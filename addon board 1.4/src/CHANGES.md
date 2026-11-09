# Changes

Firmware 1.00 (this fork) compared with the original firmware in bwack's C64 Saver 2 (`addon board 1.4/src`). Hardware changes (a capacitor, one transistor and one resistor) are described under "Hardware changes".

**AI assistance:** The firmware changes in this fork (the refactoring and the new features) were written with AI assistance, with the author directing the work and testing it on real hardware. The code has not had an independent human review, so read it before trusting it with your hardware. The results under "Test results" below were measured on real hardware (a bench supply, an electronic load, a scope and a real C64), and the protection and recovery logic was also run on a PC with stubbed hardware.

## Added

- Protection runs first in every ~11 ms tick; the output is set before any display work. (The output is the `HOLD_N` pin, which the code calls the relay, a name inherited from bwack's earlier relay board. On the addon board it drives an optocoupler that switches the Q2 MOSFET.)
- ADC sampling is interrupt-driven, alternating the AC voltage channel (200-sample windows, ~42 ms) and the AC current channel (256-sample windows, ~53 ms).
- 250 ms watchdog.
- Once the microcontroller is running, the output stays off at start-up until the first valid INA219 read. (With no 9 V AC, or for a few milliseconds around a reset, the microcontroller is not driving the output, so 5 V passes straight through and only the hardware cut-off protects it; see "Known limitations".)
- INA219 link validation: configuration is written once, the calibration register is read back (mask 0xFFFE), and bad reads count against the sensor.
- SENSOR fault: latches after 50 bad ticks (~0.5 s), cuts the output, and auto-clears after 100 good ticks (~1 s) in both HOLD and AUTO. Losing the 5 V supply also unpowers the INA219, so it shows as a SENSOR fault and clears itself when the 5 V returns.
- CKDIV8 guard: reads the low fuse at start-up and halts with `CLOCK ERROR` and the output off if the clock is divided by 8. A build-time `#error` guards F_CPU.
- LOW V warning with an adjustable threshold (4.0-4.9 V, default 4.6 V), set on the new "DC Under Volt" screen.
- Guarded manual recovery (see Changed).
- New settings screens (see "Settings menu"): DC calibration (100-800, step 10, default 400; this is the INA219 calibration register, so it scales the DC current reading, not the DC voltage), AC calibration (10-50, default 20; scales AC volts only), screen saver (1m / 5m / 10m / DIS).
  - In the original firmware both calibrations were fixed constants in the code (the INA219 calibration value of 400 in `ina219.c`, and the AC volts scale in `main.c`), so correcting a reading meant editing the source and reflashing. They can now be adjusted from the screen.
- Range-checked EEPROM loading with defaults for every setting.
- Firmware version shown on the splash screen.

## Changed

- HOLD / AUTO now governs all faults, except that a SENSOR fault always clears itself once the sensor reads correctly again. Previously HOLD / AUTO only governed over-voltage.
  - HOLD: over-voltage and over-current latch until recovered.
  - AUTO over-current: up to 3 retries, ~5-6 s apart, then it latches; the retry budget refills after ~60 s healthy.
  - AUTO over-voltage: clears when the voltage falls 0.1 V below the trip setting, after the fault has been latched for at least ~2.2 s.
- Over-current requires ~340 ms of sustained overload (leaky counter, 30 ticks). It used to trip on a single reading.
- INA219 range is 320 mV (3.2 A) with single 12-bit conversions. It was 160 mV (clips ~1.6 A) with 128-sample averaging (~68 ms).
- Over-voltage trip: default 5.4 V (was 5.5 V), range 5.1-5.5 V. The 5.5 V position is labelled `5.5HW`; it is the same level as the main board's hardware cut-off. It must be seen on 3 consecutive ticks (~34 ms) so the sub-millisecond spike when the C64 is switched off does not latch a fault.
- Manual recovery needs the fault to be latched for at least 2 s, a valid sensor, voltage below the trip point, and the button held for about a second. Otherwise the screen shows "Recover refused" with "Wait 2 seconds", "Sensor not OK" or "Voltage too high".
- Button is checked on every tick with a 3-sample debounce (was every 4th tick).
- AC current is now a true RMS value in amps. The original displayed raw peak-to-peak ADC counts. The ISR sums the samples and their squares over a 256-sample window; the display takes the variance, removes the sensor noise (`ACCUR_NOISE_VAR`), takes the square root, scales it (`ACCUR_SCALE_NUM`) and smooths lightly. Idle reads 0.00 A. With the C64 at the BASIC prompt it matched a true-RMS meter (0.62-0.64 A against 0.638 A) and no longer jumps. It takes a few seconds to settle after a load change.
- AC volts are smoothed with a 1/8 running average. At idle the raw peak-to-peak reading wandered over about 0.3 V while a true-RMS meter held within 0.02 V; with the smoothing the display spans about 0.08 V (11.94-12.02 V against 11.89 V on the meter). The reading is still peak-to-peak scaled as a sine, so it can read about 1-2 % above a true-RMS meter (waveform shape and noise), and one step of the AC calibration setting is about 5 %, too coarse to trim that.
- Display self-repair: the DC/AC labels are redrawn every display cycle and the voltage fields also redraw the blank column before the current field, so stray pixels caused by display glitches (seen occasionally when the C64 is switched on or off) clear themselves. The effect on tick length was not measured (protection is still evaluated first each tick).
- UI code moved to `ui.c` / `ui.h`.
- Shared ISR data is read inside `ATOMIC_BLOCK`.
- `eeprom_update_byte` replaces `eeprom_write_byte`.
- Makefile: size flags added, fuse programming added, duplicated `main.c` source entry removed.

## Settings menu

The original firmware had three settings screens (Overcurrent, Overvoltage Hold and Voltage Trip Point). Firmware 1.00 has seven, and the way you move through them is different.

| Screen title | Setting | Range | Default | Status |
|--------------|---------|-------|---------|--------|
| DC Current Limit | Over-current limit | 0.5-2.0 A, 0.1 A steps | 1.2 A | Was "Overcurrent" |
| Fault Recovery | HOLD or AUTO | HOLD / AUTO | HOLD | Was "Overvoltage Hold" (AUTO was "AUTORECOVER"); now governs over-voltage and over-current (a SENSOR fault always clears itself) |
| DC Volt Trip | Over-voltage trip | 5.1-5.5 V (5.5 = `5.5HW`), 0.1 V steps | 5.4 V | Was "Voltage Trippnt." |
| DC Under Volt | LOW V warning threshold | 4.0-4.9 V, 0.1 V steps | 4.6 V | New |
| DC Calibration | INA219 calibration register (scales the DC current reading) | 100-800, steps of 10 | 400 | New |
| AC Calibration | Scales the AC volts reading | 10-50, steps of 1 | 20 | New |
| Screen Saver | Time before the display blanks | 1m / 5m / 10m / DIS | 5m | New |

How the menu works:

- A short press on the dashboard opens the settings, starting at DC Current Limit. The original used a short press to step through read-only screens and a long press to edit.
- Each screen shows the title and value on top and an action bar on the bottom line: `+ - S C` (plus, minus, save, cancel). A short press moves the cursor along the bar; a long press activates the item under the cursor.
- Save writes that one setting to EEPROM and moves on to the next screen in the table's order. Save on Screen Saver returns to the dashboard.
- Cancel returns to the dashboard immediately. Settings already saved on earlier screens stay saved; only the value on the screen you cancel is discarded.
- A long press on the dashboard still goes to fault recovery, as before (see "Changed").

## Fixed

- The output was switched on during initialisation, powering the C64 before any checking. Once the microcontroller is running, it now starts off.
- A missing or dead INA219 read back as 0xFFFF, which the protection logic saw as ~32.8 V / ~65 A and tripped only by accident. The screen showed nonsense (for example `-3.276V` and `0.0-1A`) because the display code prints those values as signed numbers. It is now an explicit SENSOR fault.
- Calibration and configuration were rewritten on every bus-voltage read, restarting conversions each cycle.
- Over-current auto-retried forever, even in HOLD.
- Any long press cleared a latched fault immediately.
- EEPROM values below 255 were accepted without range checks.
- `send_byte` ignored the ACK and read `PORTB` instead of `PINB`.

## Removed

- `snprintf` (replaced by a small `formatNumber()` routine).
- Busy-wait ADC polling and the 3000-sample windows.

## Size

| Build    | Flash (of 8192 B) | Static RAM (of 512 B) | Left for stack |
|----------|-------------------|-----------------------|----------------|
| Original | 7,713 B           | 175 B                 | ~337 B         |
| Fork 1.00 | 8,147 B (99.5 %) | 88 B                  | ~424 B         |

`avr-size` for 1.00: text 8134, data 13, bss 75. Only 45 B of flash is free. Built with avr-gcc 15.1 (Microchip AVR8 toolchain). Original: text 7580, data 133, bss 42 (built with the same toolchain; its `ina219.h` has a header-guard typo, so it needs `-Wno-error=header-guard` on GCC 15). Re-check size if you use another compiler version.

## Build

The size flags are **required** to fit in 8 KB:

```
-Os -flto -mcall-prologues -ffunction-sections -fdata-sections -Wl,--gc-sections
```

plus `-Wall -Werror -Wstrict-prototypes -std=gnu99 -funsigned-char -funsigned-bitfields -fpack-struct -fshort-enums -DF_CPU=8000000UL`. The Makefile sets all of these. In PowerShell, quote `"-Wl,--gc-sections"`.

## Prebuilt firmware

A ready-to-flash `.hex` is attached to the GitHub Release. Set the fuses below, then flash the hex. It was built with avr-gcc 15.1 using the Makefile flags above.

SHA-256 of `main.hex`: `d4c6bb63944edea1eb422d865a583754a4e85759c022c8070d1e375d7e3799cd`

PowerShell: `(Get-FileHash .\main.hex -Algorithm SHA256).Hash.ToLower()`

## Fuses (ATtiny85)

| Fuse | Value | Meaning |
|------|-------|---------|
| lfuse | `0xD2` | 8 MHz internal oscillator, CKDIV8 off |
| hfuse | `0xDD` | SPIEN on, brown-out at 2.7 V (`0xDF` = brown-out off) |
| efuse | `0xFF` | default |

Warning: a wrong lfuse clock source (for example `0xDD`, external crystal) stops the chip and blocks ISP. Recovery needs a high-voltage programmer or an external clock on pin 2.

Flashing wipes the EEPROM (EESAVE is not programmed). Back it up first:

```
avrdude -p attiny85 -c usbtiny -P usb -B 5 -U eeprom:r:eeprom_backup.hex:i
```

Use `-B 10` if the chip is running at 1 MHz.

## Hardware changes

These are not changes to the repo's design files. They are modifications made to the boards used to develop and test this firmware.

- **Addon board:** a 1000 uF capacitor was added in parallel with the 3 x 47 uF bank after D1 to reduce ripple (to under ~150 mV) so the AC current sensor reads correctly.
- **Main board (through-hole 2.6), Q1 and R5:** with the addon fitted, a stock board could not turn Q2 (IRFR5305) on fully, and the C64 would not power up (Q2 gate to source about -2.7 V; 5 V collapsing to about 0.5 V at the C64). The addon's U5 optocoupler LED loads the node Q1 has to pull low, and a BC547A (hFE 176) does not have enough gain at the available base current. The node measured 2.4 V with the addon and 0.26 V without it.
  - Q1: BC547A replaced with a BC547C (hFE 500 on a component tester).
  - R5: 3.3k replaced with 100k.
  - R6, R7 and R8 are stock.
  - Result: Q1 collector 0.248 V with the addon fitted (0.086 V without it), Q2 gate to source -4.89 V, and 0.19 V input-to-output drop at the BASIC prompt (5.275 V in, 5.09 V on 5V_SAFE). The C64 starts first time, including from an overnight cold start.
  - R5 was left at 100k. A lower value would speed up the hardware cut-off slightly (estimated at about 35 us), which is small next to the measured response below. With the addon fitted, its optocoupler path supplies most of the pull-up, so R5 matters little.

## Test results

Bench results for the hardware and firmware above, on the through-hole 2.6 main board with the addon.

### Hardware over-voltage cut-off (BC547C, R5 = 100k)

Measured by oscilloscope. The scope agreed with a bench meter within about 7 mV.

| Input edge | Output peak |
|---|---|
| Slow ramp | 5.52 V |
| about 3 V/ms | 5.54 V |
| about 25 V/ms (no series resistor) | 5.58 V |

- The static trip is about 5.50 V (the circuit is not precisely characterised; treat it as 5.5 V give or take a few tens of mV).
- Once the gate moves, the output collapses within about 12-17 us.
- A transient to 5.54 V lasting about 10 us did not trip it, so the trip has a response time of 10 us or more.
- Faster edges were not tested. A straight-line extrapolation of the three points suggests an output near 5.8-5.9 V at about 100 V/ms; this is an estimate, not a measurement.

### Firmware over-voltage cut-off

- Trip 5.1 V, HOLD mode, input stepped from 5.0 V to 5.3 V, 150 ohm load, 8 runs: the Q2 gate rose 30.8-36.4 ms after the input crossed the trip point (mean 33.6 ms). That is the 3-reading confirmation at an ~11 ms tick plus up to one tick of phase. The output was below about 0.4 V within one 100 us sample of the gate moving.
- Trip accuracy: at settings of 5.1, 5.2, 5.3 and 5.4 V the firmware tripped when the meter read 5.080, 5.185, 5.283 and 5.386 V, within 0.02 V of the setting. At 5.5HW it tripped at 5.484 V on the meter.
- AUTO recovery: after an over-voltage trip at 5.1 V, lowering the input to 4.98 V cleared the fault once the latch time had passed; HOLD stayed latched.

### Over-current

- A 1.1 A load ran without tripping; a 1.3 A load tripped quickly (limit 1.2 A, default).
- AUTO retried three times and then latched; HOLD latched on the first trip.
- Because the displayed current reads about 2 % low (see below), the real trip is near 1.225 A.

### Readings against a meter

- **DC current:** the OLED reads about 2 % low compared with a series meter, from 0.1 A to 1.1 A (about 8 mA low at 0.1 A and 25 mA low at 1.1 A). At no load it shows 0.01 A, which is the board's own draw (the meter showed 0.009-0.013 A). A single 0.1 ohm shunt tolerance and the INA219's own tolerance would account for this. Treat the current as accurate to about 3 % or 25 mA.
- **DC voltage:** with no load the OLED matched the meter within 5 mV at 5.2, 5.3 and 5.4 V. Under load the OLED reads below the input terminals:

| Load | Input terminals | OLED | Output (5V_SAFE) |
|---|---|---|---|
| 0.75 A | 5.03 V | 4.81 V | 4.80 V |
| 1.0 A | 5.01 V | 4.71 V | 4.67 V |
| 1.1 A | 5.00 V | 4.67 V | 4.61 V |

  The OLED figures in this table were logged in a separate run from the input readings, so the gap between the OLED and the input terminals here should not be relied on. In this run the OLED was within 0.01-0.06 V of the output voltage. In a later test with the C64 at about 0.73 A, the OLED read 5.2 V with 5.27 V at the input and 75.2 mV across the shunt, which is the expected shunt drop only. The larger gap in the table was not reproduced, and its cause was not isolated.
- **Voltage budget:** with a 5.0 V input, input to output drops 0.23 V at 0.75 A, 0.34 V at 1.0 A and 0.39 V at 1.1 A. That is the shunt, Q2 (about 0.17-0.23 ohm at these currents, with a gate-to-source voltage of about -4.6 V) and about 0.03 ohm of connectors. With the C64 at the BASIC prompt (about 0.73 A, 5.275 V input) the drop was 0.19 V, possibly because Q2 gets more gate drive at the higher input voltage (not tested). If the earlier figures hold, at a 5.0 V input the C64 receives 4.80 V at 0.75 A, and above about 0.9 A the output falls below 4.75 V unless the supply is set higher.

### Switching with the C64

20 power-switch cycles on the C64, 10 cycles of the 5 V supply and 5 quick cycles (about 0.5 s off), then a 10 minute idle: no nuisance trips, and the C64 reached BASIC every time at a 5.4 V trip. The OLED peaked at about 0.75 A at switch-on and settled at about 0.7 A. From an overnight cold start with the addon fitted, the C64 started first time, peaking at about 0.78 A.

### Other checks

- SENSOR fault: cutting the 5 V supply shows SENSOR on the OLED, and it clears by itself when the 5 V returns, in HOLD and in AUTO.
- Menus: every setting saves and survives a power cycle; screens, the screen saver, the LOW V warning and the AC readings behaved normally.
- Protection, recovery and the CKDIV8 guard were also simulated on a PC with stubbed hardware, and brown-out detection (hfuse `0xDD`) was tested with a slow 9 V AC shutdown.
- Most of these tests were run before the smoothing on the AC volts reading was added, which does not touch the protection code. After adding it, an over-voltage trip, an over-current trip, a settings save and a C64 power cycle were repeated on the release build.
- Soak: the release build ran overnight with the C64 sitting at the BASIC prompt. No trips, no fault messages, no display problems, and the C64 was still at BASIC at the end.

## Known limitations

- 5V_SAFE is not firmware-protected while the MCU is unpowered (no 9 V AC) and for a few ms around a reset; only the hardware cut-off covers it.
- The button shares the AC voltage sense pin (< 100 counts = pressed).
- Faults are not saved across a reset.
- There is no settings lock.
- There is no read-only view of the settings. A short press on the dashboard opens the editor (the original let you step through the settings without changing them), so move to Cancel (`C`) and long-press to leave without saving.
- DC calibration steps in 10s.
- There is no calibration screen for AC current. Its scale is a build-time constant, `ACCUR_SCALE_NUM` in `board.h` (the datasheet value for an ACS712-5A is 33; 46 matched a true-RMS meter on the test unit). That was a single-point calibration: idle read 0.00 A and the BASIC prompt matched the meter, but linearity at other loads was not checked. A different sensor variant needs a different constant.
- The AC readings are display only; no protection logic uses them.
- Flash is nearly full (99.5 %, 45 B free). Adding features such as an amps calibration screen would mean trimming code elsewhere.
- A stray pixel pattern can appear occasionally in the top line when the C64 is switched on or off (likely a noise burst on the display bus). The firmware redraws the labels and the blank column every display cycle, so it clears within moments. No effect on protection. An optional 100 nF capacitor across the OLED's VCC and GND was not added, to keep the board close to stock and the OLED header free for programming.
- The hardware changes were tested on a through-hole 2.6 main board only. The SMD board was not tested. Its 2.6 schematic uses a BC847 (the surface-mount equivalent of the BC547) in the Q1 position with the same 3k3 for R5, so it may have the same problem and respond to the same fix, but this is untested. The BOM in the SMD folder still lists a 2N7002 from an earlier version.
- The firmware over-voltage threshold is compared with the OLED reading, which sits below the input terminals under load (see "Readings against a meter"). The gap is at least the shunt drop (about 75 mV at 0.75 A), so at a 5.4 V setting and about 0.75 A the input would need to be about 5.475 V, just under the ~5.5 V hardware cut-off. If the larger gap seen in the earlier electronic-load test is real, the hardware acts first.
- The INA219's supply pin is fed through a small regulator (U8) from the incoming 5 V, and its sense inputs are rated to 26 V, so a fault of around 9 V on the 5 V line does not exceed its ratings. A damaged sensor shows as a SENSOR fault and the output stays off; the hardware cut-off is what covers that case.
- The addon's 5 V regulator U7 (MCP1703A-5002E/DB, rated for 16 V in normal operation and 18 V absolute maximum) is fed from the rectified 9 V AC rail. On the test unit that rail measured 15 V DC at idle, so it is only about 1 V from the 16 V limit, and there is little margin if the mains runs high or the supply puts out more than the test transformer (11.8 V RMS at idle). This comes from the original design, not from the firmware. A future rework of the board should consider changes in this area, such as a regulator with a higher input rating (which may also need a larger output capacitor).
- The schematics in the repository do not show the R5 change or the 1000 uF capacitor added to the addon board.

## Credits and licence

Original hardware and firmware by bwack. Repository licence: CERN OHL v1.2. OLED driver: tinusaur ssd1306xled library (MIT).
