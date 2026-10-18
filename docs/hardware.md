# MeshCore Pager - hardware

Read off the EasyEDA schematic (sheets 1-7, `Schematic_2023-K-167.pdf`),
which is not kept in this repo - this file is the part of it the firmware actually depends on. `include/pins.h` is the
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
| Charger CHG# sense | GP13 | Original: R21 330R from the cell, D5 3V3 zener to CHG# - reads HIGH whenever a cell is fitted. Reworked: CHG# direct, internal pull-up. See below |
| USB power sense | GP24 | Reworked boards only: VCCFILTER -> 100k -> GP24 -> 150k -> GND. Floating on the original |
| Battery sense | GP27 | ADC1, R12/R9 100k/100k -> Vbat/2 |

## What the firmware has to work around

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

**No way to tell charging from on battery.** GP13 was meant as the charge
sense and does not work as one. R21 runs from the BQ21040's OUT - which is
the cell, not USB - to GP13, and D5 (BZT52C3V3, cathode to GP13, anode to
CHG#) is the only link to CHG#. With CHG# low the zener clamps GP13 to about
3 V; with CHG# open GP13 sits at the cell voltage behind 330R, held near
3.6 V by the RP2040's own clamp diode. Both read HIGH. There is no VBUS
sense on any GPIO either, so the only sign of USB power is a USB host
answering (`tud_ready()`), which a plain charger or a sleeping laptop does
not give.

**The rework that fixes it**, and that the firmware detects:

```
R21               removed
D5                bridged (or 0R)    GP13 = CHG#, internal pull-up, LOW = charging
VCCFILTER ──[100k]──┬── GP24         HIGH (3.0 V from 5 V) = USB power
                    └──[150k]── GND
```

CHG# is open-drain, rated to 7 V against VSS whatever IN is doing, and
high-impedance when the charger is unpowered (BQ21040 datasheet 7.1, 8.3.1),
so it needs a pull-up and nothing else; TI's own advice for a processor is
exactly that (9.2.2.1.4). The zener D5 was presumably the datasheet's 6.2 V
clamp (8.4.1), which is only for a pull-up source above 7 V. The divider
costs the battery nothing: VCCFILTER is dead off USB, D6 blocking the way
back. Its 60k source impedance is why the firmware probes for it by driving
GP24 and letting go rather than with a pull - see `probePowerSense()`.

CHG# is low for the first charge after power arrives and stays off through
later top-ups (8.3.4, 8.4.10), so off-and-on-USB means full. Plugged into a
full battery it goes low for up to a minute while the charger checks
(8.4.11). It also stays low through a temperature fault on the NTC (table
1), and goes off when a safety timer expires (8.4.7) - neither likely here.

**The battery reading has a floor.** The power path is Q1 (SI2301A): with
USB present D6/D7 feed LOAD from VCCFILTER and hold Q1 off, so the cell
only sees the charger; on battery R10 pulls Q1's gate down and LOAD is the
cell. LOAD feeds the XC6206 LDO that makes +3V3, and +3V3 is also ADC_AVDD,
the ADC's reference. Once the cell is within the LDO's dropout of 3.3 V -
about 3.4 V at the pager's idle current, higher while transmitting - the
reference falls with the cell, the ratio the ADC measures stops changing,
and the reading sticks at about 3.4 V. The XC6206's ±2% output tolerance is
also the reading's: ±80 mV at 4 V. The divider (100k/100k, 50k source)
works only because C7/C19 hold 200 nF on the ADC pin; it takes tens of
milliseconds to settle after the cell has sagged under a transmission.

**No RTC and no GPS.** Both time and position come off the mesh. See the
README for what that means in use, and `include/pagerclock.h` and
`include/grid.h` for how.

## Settled by the previous firmware, not by the schematic

[LoRa433APRSpager](https://github.com/Guru-RF/LoRa433APRSpager) is
CircuitPython for this same board and ran on it. Four things the schematic
does not say, or says differently, that its `code.py` does:

- **The panel is mounted upside down.** It sets `rotation=180`; `src/ui.cpp`
  uses `U8G2_R2`. U8g2 applies the SH1106's 2-column offset in the tile
  layer, below the rotation, so the offset stays correct either way up.
- **The buzzer is wideband, not a 2.7 kHz resonator.** It is driven from
  `pwmio` at a 50% duty cycle and swept from 330 Hz to 1165 Hz. That is the
  range `src/alerts.cpp` uses, and the alarm reproduces that sweep exactly.
- **GP13 had a pull-down.** It set `Pull.DOWN` on the charge sense. On the
  original board the pad is never floating - R21 holds it up to the cell -
  so the pull-down did nothing but draw from the battery. On a reworked
  board CHG# needs the opposite, a pull-up, which is what this firmware sets.
- **The LED order is R, G, B on GP7, GP8, GP9** - it constructs
  `RGBLED(GP7, GP8, GP9, invert_pwm=True)`, which also confirms the
  common-anode inversion.

Battery scaling agrees too: it computes `value * 3.3 / 65536 * 2`, the same
half-divider this firmware assumes.
