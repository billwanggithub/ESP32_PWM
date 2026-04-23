#include "dns_hijack.h"

#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dns_hijack";

static volatile TaskHandle_t s_task;
static int          s_sock = -1;
static volatile bool s_run;

// AP gateway IP. Must match esp_netif's default SoftAP config (192.168.4.1).
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

// Build the A-record answer appended after the question section.
// Uses name compression (0xC00C) pointing back at the question name.
// RR wire format: NAME(2) TYPE(2) CLASS(2) TTL(4) RDLENGTH(2) RDATA(4) = 16 bytes.
static int append_answer(uint8_t *out, int question_end_off)
{
    uint8_t *p = out + question_end_off;
    *p++ = 0xC0; *p++ = 0x0C;               // compressed name → offset 12
    *p++ = 0x00; *p++ = 0x01;               // TYPE = A
    *p++ = 0x00; *p++ = 0x01;               // CLASS = IN
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  // TTL = 60
    *p++ = 0x00; *p++ = 0x04;               // RDLENGTH = 4
    memcpy(p, AP_IP, 4); p += 4;
    return (int)(p - (out + question_end_off));
}

// Walk the first question's NAME field to find where the question section ends.
// Returns offset just past the QTYPE/QCLASS pair, or -1 on malformed input.
static int question_end_offset(const uint8_t *buf, int len)
{
    int off = 12;                            // skip DNS header
    while (off < len) {
        uint8_t l = buf[off];
        if (l == 0) { off += 1; break; }
        if ((l & 0xC0) == 0xC0) { off += 2; break; }  // compressed, unusual in queries
        off += 1 + l;
    }
    off += 4;                                // QTYPE + QCLASS
    return off <= len ? off : -1;
}

static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in peer;
    socklen_t peer_len;

    while (s_run) {
        peer_len = sizeof(peer);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&peer, &peer_len);
        if (n < 12) continue;                // need at least a header

        int qend = question_end_offset(buf, n);
        if (qend < 0 || qend + 16 > (int)sizeof(buf)) continue;

        // Flip flags: QR=1 response, Opcode=query (leave), RA=1, RCODE=0.
        buf[2] = 0x80 | (buf[2] & 0x01);     // QR=1, OPCODE=0, AA=0, TC=0, RD=copy-from-query
        buf[3] = 0x80;                       // RA=1, RCODE=0
        buf[6] = 0x00; buf[7] = 0x01;        // ANCOUNT = 1
        buf[8] = 0x00; buf[9] = 0x00;        // NSCOUNT = 0
        buf[10] = 0x00; buf[11] = 0x00;      // ARCOUNT = 0

        int ans_len = append_answer(buf, qend);
        sendto(s_sock, buf, qend + ans_len, 0,
               (struct sockaddr *)&peer, peer_len);
    }

    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(void)
{
    if (s_sock >= 0) return ESP_ERR_INVALID_STATE;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) return ESP_FAIL;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }

    // 500ms recv timeout so the task can poll s_run during shutdown.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    if (setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGE(TAG, "setsockopt SO_RCVTIMEO failed: errno=%d", errno);
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }

    s_run = true;
    // xTaskCreate wants a non-volatile handle pointer. Stash into a local
    // and assign after creation — the volatile qualifier on s_task is for
    // the cross-core READ in dns_hijack_stop, not this one-time write.
    TaskHandle_t h = NULL;
    if (xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 2, &h) != pdPASS) {
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }
    s_task = h;
    ESP_LOGI(TAG, "dns hijack up (UDP:53 → 192.168.4.1)");
    return ESP_OK;
}

void dns_hijack_stop(void)
{
    if (!s_run) return;
    s_run = false;
    // Task will exit within one recv timeout (~500 ms) and self-delete.
    for (int i = 0; i < 20 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
