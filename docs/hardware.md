# MeshCore Pager - hardware

Read off the EasyEDA schematic (sheets 1-7), which is not kept in this
repo - this file is the part of it the firmware actually depends on. `include/pins.h` is the
same table in code; this file is the reasoning behind it.

## Blocks

| Sheet | Block |
|---|---|
| 1 | RP2040, 12 MHz crystal, W25Q16 QSPI flash, BOOTSEL |
| 2 | XC6206P332MR LDO: LOAD -> +3V3 |
| 3 | USB-C receptacle, ESD array, ferrite/CC pulldowns |
| 4 | OLED header, buzzer, RGB LED, button |
| 5 | RFM96W LoRa module + SMA |
| 6 | BQ21040 LiPo charger, CHG# sense |
| 7 | Battery, load switch, battery divider to ADC1 |

## Pin map

| Function | GPIO | Notes |
|---|---|---|
| LoRa MISO | GP16 | spi0 RX |
| LoRa SCK | GP18 | spi0 SCK |
| LoRa MOSI | GP19 | spi0 TX |
| LoRa NSS | GP0 | |
| LoRa RESET | GP1 | |
| OLED D0 (clock) | GP10 | software SPI |
| OLED D1 (data) | GP11 | software SPI |
| OLED CS | GP21 | |
| OLED DC | GP20 | |
| OLED RES | GP22 | |
| Button (SW2) | GP12 | to ground, needs the internal pull-up |
| Buzzer (U1) | GP15 | driven directly, no transistor |
| LED red | GP7 | common anode via R1 470R, active LOW |
| LED green | GP8 | " |
| LED blue | GP9 | " |
| Charger CHG# sense | GP13 | via R21 330R + D5 3V3 zener |
| Battery sense | GP27 | ADC1, R12/R9 100k/100k -> Vbat/2 |

## Three things the firmware has to work around

**No radio interrupt.** DIO0..DIO5 on the RFM96W are all unconnected, so
there is no RxDone or TxDone line. Both directions poll the chip's IRQ flag
register (`src/radio.cpp`). Receive is continuous mode, so RxDone latches and
a late poll still finds the packet - what a late poll costs is the *second*
packet of a back-to-back pair. At SF8/62.5 kHz a frame is roughly a second on
air and the main loop runs every few milliseconds, so that is not close.

**The display cannot use spi1.** GP10/GP11 are spi1 pins, so the panel cannot
share spi0 with the radio - but this core's spi1 defaults are MISO=GP12,
MOSI=GP15, SS=GP13, which on this board are the button, the buzzer and the
charge sense. Any `SPI1.begin()` reconfigures all three. The display is
therefore bit-banged on GP10/GP11 and nothing else is touched. (U8g2's
second-hardware-SPI constructors are not compiled on this core anyway - it
does not define `SPI_INTERFACES_COUNT`.)

**No RTC and no GPS.** Both time and position come off the mesh. See the
README for what that means in use, and `include/pagerclock.h` and
`include/grid.h` for how.

## Settled by the previous firmware, not by the schematic

[LoRa433APRSpager](https://github.com/Guru-RF/LoRa433APRSpager) is
CircuitPython for this same board and ran on it. Four things the schematic
does not say, that its `code.py` does:

- **The panel is mounted upside down.** It sets `rotation=180`; `src/ui.cpp`
  uses `U8G2_R2`. U8g2 applies the SH1106's 2-column offset in the tile
  layer, below the rotation, so the offset stays correct either way up.
- **The buzzer is wideband, not a 2.7 kHz resonator.** It is driven from
  `pwmio` at a 50% duty cycle and swept from 330 Hz to 1165 Hz. That is the
  range `src/alerts.cpp` uses, and the alarm reproduces that sweep exactly.
- **GP13 needs a pull-down.** It sets `Pull.DOWN` on the charge sense; off
  charge the pad is floating and would otherwise read as whatever it last
  saw.
- **The LED order is R, G, B on GP7, GP8, GP9** - it constructs
  `RGBLED(GP7, GP8, GP9, invert_pwm=True)`, which also confirms the
  common-anode inversion.

Battery scaling agrees too: it computes `value * 3.3 / 65536 * 2`, the same
half-divider this firmware assumes.
