# Wiring and GPIO budget (ESP32-S3 N16R8 camera dev board)

## Camera (fixed by the board, ESP32-S3-EYE-compatible map)
| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| XCLK | 15 | D0..D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| SCCB SDA / SCL | 4 / 5 | VSYNC / HREF / PCLK | 6 / 7 / 13 |
PWDN and RESET are not connected (-1) - the sensor cannot be power-cycled by software.

## Other pins in use or reserved
| GPIO | Use |
|---|---|
| 43 / 44 | console UART0 (TX / RX) |
| 19 / 20 | native USB |
| 26-32, 35-37 | flash / octal PSRAM on N16R8 modules: not available |
| 0, 3, 45, 46 | strapping pins: avoid for outputs that can be pulled at boot |
| **1 / 2** | **AS608 (default, changeable in menuconfig: "AS608 fingerprint sensor")**: ESP TX = 1, ESP RX = 2 |

Candidates for the relay, LEDs, buzzer and button (Phase 3): 14, 21, 38-42, 47, 48 - **verify against your board's pin
header and schematic first**; some boards route these to an SD slot or an RGB LED.

## AS608 fingerprint module
```
module TX  ->  ESP32 RX (GPIO 2)          module RX  ->  ESP32 TX (GPIO 1)
module GND ->  ESP32 GND                   module VCC ->  see below
```
- **Colours differ between vendors. Read the silkscreen / datasheet of your module; do not assume.**
- AS608 datasheets normally give a **3.6-6 V supply with 3.3 V logic** (about 120 mA while capturing). Power it from the 5 V
  rail with a common ground. Some clones differ: **before connecting the data lines, measure the module's TX idle level
  with a multimeter. If it is near 5 V, do not connect it to the ESP32 directly (use a level shifter or divider).**
- Default protocol settings: 57600 baud, 8N1, address 0xFFFFFFFF, password 0x00000000.
- The module has a touch/wake pin (finger detect). It is unused for now; the driver polls.
