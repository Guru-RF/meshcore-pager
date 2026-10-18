// ============================================================
// RF.Guru MeshCore Pager - Hardware pin definitions
//
// Read off the board schematic (docs/hardware.md). Everything here is
// fixed by copper; nothing in this file is configurable.
// ============================================================

#pragma once

// ---- LoRa: HopeRF RFM96W (Semtech SX1276) on SPI0 ----
//
// GP16/GP18/GP19 are the only spi0 RX/SCK/TX pins available, so the radio
// owns SPI0 and the display cannot share it.
#define PIN_LORA_MISO   16
#define PIN_LORA_SCK    18
#define PIN_LORA_MOSI   19
#define PIN_LORA_CS      0
#define PIN_LORA_RST     1

// The module's DIO0..DIO5 are all left unconnected on this board, so there
// is no RxDone or TxDone interrupt. Both directions poll the chip's IRQ
// flag register instead (src/radio.cpp). Set this to a GPIO if a future
// revision wires DIO0 up; nothing else has to change.
#define PIN_LORA_DIO    -1

// ---- Display: 1.3" 128x64 mono OLED, 7-pin SPI module, on SPI1 ----
//
// GP10 is spi1 SCK and GP11 is spi1 TX; that is what puts the display on
// the second SPI block. A 1.3" panel is an SH1106 in practice - the 0.96"
// ones are SSD1306 - so that is the default; -DPAGER_OLED_SSD1306 switches
// the controller without touching anything else.
#define PIN_OLED_SCK    10      // module pin 3, "D0"
#define PIN_OLED_MOSI   11      // module pin 4, "D1"
#define PIN_OLED_CS     21      // module pin 7
#define PIN_OLED_DC     20      // module pin 6
#define PIN_OLED_RST    22      // module pin 5

// ---- Button (SW2) ----
//
// A plain tactile switch to ground with no external pull-up, so the pad
// needs the RP2040's internal one and reads LOW when pressed.
#define PIN_BUTTON      12

// ---- Buzzer (U1, CUI CST-931AP) ----
//
// A transducer straight off the GPIO, no driver transistor: it needs a
// square wave, which is what tone() produces. It is NOT the narrow-resonance
// part its package suggests - the previous firmware for this board
// (github.com/Guru-RF/LoRa433APRSpager) drove it from a 50%-duty pwmio and
// swept it 330 Hz to 1165 Hz, and that is the range src/alerts.cpp uses.
// tonePitch in config.txt shifts the whole set if a different part is fitted.
#define PIN_BUZZER      15

// ---- Indicator LED (LED1) ----
//
// Three dice in one package with their anodes commoned through R1 (470R)
// to +3V3, so a channel lights when its GPIO is driven LOW and the three
// share one current budget - lighting all three is dimmer per colour than
// lighting one. The firmware drives them with analogWrite() and inverts,
// so 0 is off and 255 is full brightness (see src/alerts.cpp).
//
// The colour order is not on the schematic but is settled by the previous
// firmware, which constructed its LED as RGBLED(GP7, GP8, GP9, invert_pwm).
#define PIN_LED_R        7
#define PIN_LED_G        8
#define PIN_LED_B        9

// ---- Battery and charger ----
//
// R12/R9 form a 100k/100k divider from the cell to ADC1, so the pad sees
// half the battery voltage; C7/C19 already filter it.
#define PIN_VBAT_ADC    27      // ADC1
#define VBAT_DIVIDER    2.0f

// The BQ21040's CHG#, and two boards to tell apart (docs/hardware.md):
//
//   reworked  R21 removed, D5 bridged: GP13 is CHG# itself. Open-drain, so
//             it needs the internal pull-up, and reads LOW while charging.
//   original  R21 (330R) from the cell to GP13, D5 (3V3 zener, cathode to
//             GP13) to CHG#. It reads HIGH whenever a cell is fitted,
//             charging or not, and is not read.
#define PIN_CHARGE      13

// Reworked boards only: VCCFILTER -> 100k -> GP24 -> 150k -> GND, so it reads
// HIGH (3.0 V from 5 V) while USB power is present. Floating on the original
// board, which is how the firmware tells the two apart - probePowerSense().
#define PIN_VBUS        24
