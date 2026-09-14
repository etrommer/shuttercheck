# shuttercheck

Camera shutter speed tester. An SFH 309 FA phototransistor senses the light
that goes through the shutter. A Black Pill (STM32F103C8T6) samples the
phototransistor output with its own 12-bit ADC on a timer-driven 2 µs grid.
The firmware rebuilds both shutter edges from the sample stream by
interpolation. There is no external comparator and no trimpot. The firmware
sends the measured exposure over USB CDC. Any serial terminal can read it.

[![PlatformIO CI](https://github.com/etrommer/shuttercheck/actions/workflows/ci.yml/badge.svg)](https://github.com/etrommer/shuttercheck/actions/workflows/ci.yml)

The capture path operates at this time. The firmware samples PA1 on the
timer-driven grid and sends the raw counts over USB CDC as `adc <counts>`
lines, ten reports each second. The crossing scan and the shutter measurement
are not written. Thus the firmware does not print the exposure. `AGENTS.md`
holds the build and flash commands and the design invariants.

## How it measures

1. **SFH 309 FA** (Si NPN phototransistor, T1 3 mm radial). Connect the
   collector to 3V3 and the emitter to the load resistor `R_E` to GND. The
   current gain of the transistor multiplies the photocurrent. Thus the emitter
   node moves by hundreds of mV in normal test light. The node drives
   **PA1 = ADC1_IN1** through a 100 Ω series resistor. This resistor is the
   complete analog front end.
2. **Sampling grid.** A hardware timer starts one 12-bit ADC conversion each
   **2.000 µs** (500 kS/s). The sampling instants are hardware events. No
   software can move them.
3. **ADC and DMA.** The DMA writes each conversion result into a circular
   buffer when it arrives. Interrupt latency can never drop or retime the
   stream.
4. **Crossing detection.** The firmware scans each buffer chunk when it
   arrives. It finds the exact moment when the voltage crossed the threshold.
   It uses linear interpolation between the two samples that straddle the
   crossing:

   ```
   t_cross = (n − 1) × 2 µs + (V_thr − V_{n−1}) / (V_n − V_{n−1}) × 2 µs
   ```

   The firmware tracks the **dark plateau** (between
   pulses) and the **bright plateau** (during the pulse). It crosses at their
   midpoint. A software hysteresis band stops noise from faking an edge. The
   band is max(8 LSB, one sixteenth of the bright−dark span). A pulse re-arms
   only after the signal falls back to the low band edge. The firmware
   estimates the plateaus from samples that are not in a crossing pair and not
   in the band.
5. **Exposure.** The exposure is `t_cross(falling) − t_cross(rising)`. A
   sample is valid only if it has exactly one rising and one falling crossing.
   There are two rejection paths:
   - `clipped`: the bright plateau is within 32 LSB of full scale. The front
     end is saturated. The comparator design cannot see this fault.
   - `weak`: the bright−dark span is less than 48 LSB (≈40 mV). The midpoint
     would be noise.

There is **no range state machine**. One constant 500 kS/s grid covers all
speeds. At 1/4000 the pulse still gets ~125 samples for the interpolation. A
6 s Bulb is only a longer stream. Nothing can overflow.

| Speed     | Exposure | Samples/pulse | Raw grid error | With interpolation |
|-----------|----------|---------------|----------------|--------------------|
| 1/4000    | 250 µs   | 125           | ±0.8 %         | ≈0.05 % (noise-limited) |
| 1/1000    | 1 ms     | 500           | ±0.2 %         | ≈0.02 %            |
| 1/30      | 33 ms    | 16 700        | ±0.006 %       | threshold noise    |

Rated spec: **1/4000 … 6 s**. Faster speeds show correctly if the edges
resolve. But they are outside the accuracy budget below.

Polarity convention: dark gives a low voltage on PA1, light gives a high
voltage. The rising crossing is the shutter opening. The falling crossing is
the shutter closing. The phototransistor is an emitter follower. Thus the
front end is non-inverting.

### Output

One line for each capture on the USB CDC port. The line holds nanoseconds and
then a status.

```
2481234 ok
0 stale
0 clipped
0 weak
```

| Status    | Meaning                                                              |
|-----------|----------------------------------------------------------------------|
| `ok`      | One rising + one falling crossing, verified plateaus; count is final |
| `stale`   | A falling crossing arrived without a rising crossing of its own pulse |
| `clipped` | Bright plateau within 32 LSB of full scale: saturated front end; discarded |
| `weak`    | Bright−dark span below the noise floor; the threshold would be meaningless; discarded |

The LED on PB12 gives one short flash for each accepted sample. It stays dark
for a rejection. `2481234 ns` = 2.481 ms ≈ 1/403 s. Send the output to any
serial terminal (`pio device monitor`). No host-side software is necessary.

## Wiring

The complete front end operates from **3V3**. Keep 5 V away from the MCU pin.
PA1 is an ADC pin. Never let the node go above 3V3 + 0.3 V.

| From                        | To                                | Notes |
|-----------------------------|-----------------------------------|-------|
| SFH 309 FA collector        | 3V3                               | Long lead |
| SFH 309 FA emitter          | node A                            | Pin 1 / short lead. Point the lens at the shutter. The half angle is only ±12° |
| `R_E` 1 kΩ                  | node A → GND                      | Controls amplitude and speed. 1 kΩ is the t_r/t_f condition in the datasheet. 100 Ω is faster, but it needs more light |
| Series 100 Ω                | node A → PA1                      | Mandatory. Limits the ADC sampling-charge kick and the fault current. The ADC sample time is dimensioned for the total ≈1.1 kΩ source impedance |
| 1 nF (optional)             | PA1 → GND, at the pin             | HF noise only. Do not use 100 nF here: with `R_E` = 1 kΩ, that value gives a 100 µs time constant and it smears a 250 µs pulse. 1 nF adds ≈1 µs of delay to both edges. The delay cancels in the width |
| Black Pill                  | USB → PC                          | The board 3V3/GND supply the front end |

Front-end sizing:

- **Keep the emitter plateau between 0.3 V and 1 V.** At 3 V the
  phototransistor is saturated. `V_CEsat` is 200 mV and the stored base charge
  delays the turn-off. Thus the closing edge reads late and fast exposures look
  long. The ADC makes this failure visible, unlike the comparator design. The
  firmware sees samples near full scale and prints `clipped`. Dim the source
  (lens aperture, distance, diffuser) until the plateau reads well below full
  scale.
- **Noise floor.** One LSB is 0.8 mV. The effective resolution of the F103 is
  closer to 9–10 bits. Thus the firmware rejects a plateau span below ~40 mV as
  `weak`. It does not report a meaningless midpoint.
- **Light source spectra.** The sensitivity peaks at 900 nm
  (range 730…1120 nm). Daylight, tungsten and xenon flash are ideal. White LEDs
  are weak at that wavelength.

## Accuracy

| Term                      | Contribution |
|---------------------------|--------------|
| 8 MHz HSE crystal         | ±30 ppm → 0.003 %. Negligible |
| Sample grid               | ±2 µs worst case between raw samples: 0.8 % at 1/4000 and 0.2 % at 1/1000. This value is the upper bound without interpolation. It is not the operating error |
| Interpolation residual    | Crossing jitter ≈ 1 LSB of voltage noise ÷ edge slope. With optical edges of ~20 mV/µs, that is tens of ns for each edge. The grid has no jitter. Conversions start on timer events |
| Phototransistor t_r/t_f   | 5…9 µs, dependent on the bin (datasheet: I_C = 1 mA, V_CC = 5 V, R_L = 1 kΩ). Both edges have about the same delay. Thus the delay largely cancels in the width. The residual comes from rise/fall asymmetry and saturation recovery |
| Optical edge shape        | **Dominant at fast speeds.** At 1/4000 the slit is only a little wider than the chip. Thus the light at the sensor is a trapezoid with edges tens of µs wide. Measurement between equal-fraction crossings cancels this shape. The auto-midpoint threshold is an equal-fraction crossing. The firmware computes it again for each pulse, instead of a trimpot set by eye |
| Plateau estimation        | The midpoint is only as good as its two plateaus. A light level that changes during a pulse or between pulses (notably xenon flash decay) biases `V_thr` and the width. The result is reproducible only with a steady source |

Bottom line: ~0.1 % class from 1/30 to 1/2 with a repeatable setup. Expect a
few percent at 1/4000. At that speed the geometry of the shutter slit dominates
and the sampling chain adds a few × 0.1 %. Remember the sign of the error: a
saturated front end biases each fast reading long. But the firmware also prints
`clipped`. That status is the reason to sample the voltage and not only to
compare it.

### What it does not measure

- **One point in the frame.** A focal-plane shutter exposes different points at
  different times. The sensor sees the light at its own position. Thus you get
  the local exposure time, not the curtain-travel uniformity across the frame.
- Flash sync timing. Also nothing about the lens aperture.
- Absolute exposure. A midpoint between two plateau estimates is not a
  photometric standard. Use the tester for comparison: same setup, compare
  against nominal or against another camera body.

## Build, flash, run

Requires PlatformIO (`pipx install platformio`, or the VS Code extension).

```sh
pio run                 # build the default env, blackpill_f103c8
pio run -e blackpill_f103c8_128   # build a 128 KiB clone
pio run -t upload       # flash over ST-Link (SWD: 3V3, GND, PA13 = SWDIO, PA14 = SWCLK)
pio device monitor      # read the reports from /dev/ttyACM0
```

### USB permissions for the upload

On Linux, `pio run -t upload` fails with `LIBUSB_ERROR_ACCESS` if your user
cannot open the ST-Link device. Install a udev rule:

```sh
sudo tee /etc/udev/rules.d/60-stlink.rules >/dev/null <<'EOF'
# ST-Link/V2 programmers: give the users group write access.
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3748", MODE:="0660", GROUP:="users"
EOF
sudo udevadm control --reload && sudo udevadm trigger
```

Then unplug the ST-Link and plug it in again. The USB CDC serial port
(`0483:5740`) usually needs no rule. If `pio device monitor` cannot open
`/dev/ttyACM0`, add your user to the `uucp` group and log in again:
`sudo usermod -aG uucp $USER`.

No ST-Link and no stm32duino bootloader? The ROM bootloader of the F103 needs
no extra hardware, but it does not operate over USB. Do these steps:

1. Wire a USB-serial adapter (3V3, GND, TX→PA10, RX→PA9).
2. Set `upload_protocol = serial` in `platformio.ini`.
3. Put the BOOT0 jumper at 1.
4. Reset the board.
5. Upload the firmware.
6. Return BOOT0 to 0 and reset the board again.

`dfu` is not in the upload protocol list of this board. A USB upload first
needs the stm32duino (Maple) bootloader at 0x08002000. Even then the board
enumerates as `1EAF:0003`, not as the ST ROM DFU device. Full command matrix
and design invariants: [`AGENTS.md`](AGENTS.md).

### Board notes to know before you blame the firmware

The STM32F103 has no internal D+ pull-up. Thus the board must have **1.5 kΩ
from PA12 to 3V3**. The Black Pill has this resistor. Unlike the Blue Pill,
there is no "measure `R10` before debugging" trap here. A dead CDC port points
to the firmware or the cabling. The regulator is an ME6211 (3.3 V, 180 mA).
That value is sufficient because the front end takes ~1 mA. Use
`blackpill_f103c8_128` in the board list if your board has the 128 KiB part.
The user LED is **PB12** on the Black Pill (PC13 on the Blue Pill). It is
active-low and sinks through the pin. Thus `LOW` lights it.
