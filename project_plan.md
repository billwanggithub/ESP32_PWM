# design a PWM, RPM capture using ESP32-S3-N16R8 module

## hardware

- ESP32-S3-N16R8 module
- android phone
 
## Implementation

PWM Generator module

- Input: PWM frequency, duty input
- no glitches when frequency or duty changes
- frequency up to 1000000Hz
- duty up to 100%
- a trigger output when frequency or duty changed

RPM capture module

- inputs: pole count, moving average count
- output: RPM
- capture RPM frequency by input capture(32 bits)
- capture RPM cycle by cycle to a FIFO queue up to 128 count
- convert RPM by frequency * 60 / pole count

Control

- by an android phone using wifi and BLE
- WEB based Control dashboard
- control PWM frequency and duty
- receive RPM capture
- can update firmware over the air
- the firmware was encrypted on the ESP32 side, can not copy by other people
