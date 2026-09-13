# shuttercheck

Camera shutter speed tester. An SFH 309 FA phototransistor watches the light
that passes through the shutter, a Black Pill (STM32F103C8T6) samples its
output directly with the MCU's own 12-bit ADC on a timer-driven 2 µs grid, and
both shutter edges are reconstructed from the sample stream by interpolation.
No external comparator, no trimpot. The measured exposure is printed over USB
CDC; any serial terminal reads it.

[![PlatformIO CI](https://github.com/etrommer/shuttercheck/actions/workflows/ci.yml/badge.svg)](https://github.com/etrommer/shuttercheck/actions/workflows/ci.yml)

The firmware is a Hello World sketch at this time. It lights the LED and
prints a line over USB CDC. The capture path is not written yet.
`AGENTS.md` holds the build/flash commands and the design invariants the
firmware must satisfy.

## How it measures

1. **SFH 309 FA** (Si NPN phototransistor, T1 3 mm radial): collector to 3V3,
   emitter to the load resistor `R_E` to GND. The photocurrent is multiplied by
   the transistor's current gain, so the emitter node swings by hundreds of mV
   under normal test light. The node drives **PA1 = ADC1_IN1** through a 100 Ω
   series resistor — that is the whole analog front end.
2. **TIM3** runs on the 72 MHz APB1 timer clock with `PSC = 143`, `ARR = 0` and
   `TRGO = update`, so it triggers one ADC conversion every exactly 144 ticks =
   **2.000 µs**. The sampling instants are hardware events; no software can
   shift them.
3. **ADC1 + DMA**: 12 MHz ADCCLK, 7.5-cycle sample + 12.5-cycle convert =
   1.67 µs per conversion, which fits inside the 2 µs trigger period. Every
   conversion is written into a circular DMA buffer by DMA — the stream is
   never dropped or re-timed by interrupt latency.
4. Crossing detection scans each buffer chunk as it lands and finds the exact
   moment the voltage crossed the threshold, by linear interpolation between
   the two samples that straddle it:

   ```
   t_cross = (n − 1) × 2 µs + (V_thr − V_{n−1}) / (V_n − V_{n−1}) × 2 µs
   ```

   `V_thr` is not a trimpot: the firmware tracks the **dark plateau** (between
   pulses) and the **bright plateau** (during the pulse) and crosses at their
   midpoint, with a software hysteresis band so noise cannot fake an edge.
5. The exposure is `t_cross(falling) − t_cross(rising)`. A sample is valid only
   if exactly one rising and one falling crossing belong to it. Rejections: a
   bright plateau within 32 LSB of full scale is `clipped` (saturated front
   end — the comparator design could not see this), and a bright−dark span
   below 48 LSB (≈40 mV) is `weak` (the midpoint would be noise).

There is **no range state machine**: one constant 500 kS/s grid covers
everything. At 1/4000 the pulse still gets ~125 samples for the interpolation
to work with, and a 6 s Bulb is just a longer stream. Nothing can overflow.

| Speed     | Exposure | Samples/pulse | Raw grid error | With interpolation |
|-----------|----------|---------------|----------------|--------------------|
| 1/4000    | 250 µs   | 125           | ±0.8 %         | ≈0.05 % (noise-limited) |
| 1/1000    | 1 ms     | 500           | ±0.2 %         | ≈0.02 %            |
| 1/30      | 33 ms    | 16 700        | ±0.006 %       | threshold noise    |

Rated spec: **1/4000 … 6 s**. Faster speeds still display correctly if the
edges resolve, but they sit outside the accuracy budget below.

Polarity convention: dark → low voltage on PA1, light → high. The rising
crossing is the shutter opening, the falling crossing the shutter closing. The
phototransistor is an emitter follower, so the front end is non-inverting.

### Output

One line per capture on the USB CDC port: nanoseconds, then a status.

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

`2481234 ns` = 2.481 ms ≈ 1/403 s. Feed it to any serial terminal
(`pio device monitor`); no host-side software is required.

## Wiring

The whole front end runs from **3V3** — no 5 V anywhere near the MCU pin, and
PA1 is an ADC pin: never let the node exceed 3V3 + 0.3 V.

| From                        | To                                | Notes |
|-----------------------------|-----------------------------------|-------|
| SFH 309 FA collector        | 3V3                               | Long lead |
| SFH 309 FA emitter          | node A                            | Pin 1 / short lead. Aim the lens at the shutter; half angle is only ±12° |
| `R_E` 1 kΩ                  | node A → GND                      | Sets amplitude *and* speed. 1 kΩ is the datasheet's t_r/t_f condition; 100 Ω is faster but needs more light |
| Series 100 Ω                | node A → PA1                      | Mandatory. Limits the ADC sampling-charge kick and fault current; the total ≈1.1 kΩ source impedance is what the 7.5-cycle sample time is sized for |
| 1 nF (optional)             | PA1 → GND, at the pin             | HF noise only. **Do not use 100 nF here**: with `R_E` = 1 kΩ that is a 100 µs time constant and it smears a 250 µs pulse. 1 nF adds ≈1 µs of delay to *both* edges, which cancels in the width |
| Black Pill                  | USB → PC                          | Board 3V3/GND feed the front end |

Front-end sizing that matters:

- **Keep the emitter plateau around 0.3…1 V.** At 3 V the phototransistor is
  saturated: `V_CEsat` is 200 mV and the stored base charge delays turn-off, so
  the closing edge reads late and fast exposures look *long*. Unlike the
  comparator design, the ADC makes this failure *visible*: the firmware sees
  samples near full scale and prints `clipped`. Dim the source (lens aperture,
  distance, diffuser) until the plateau reads comfortably below full scale.
- **Noise floor.** One LSB is 0.8 mV and the F103's effective resolution is
  closer to 9–10 bits, so a plateau span below ~40 mV is rejected as `weak`
  rather than reported on a meaningless midpoint.
- **Light source spectra.** Sensitivity peaks at 900 nm (range 730…1120 nm):
  daylight, tungsten and xenon flash are ideal, white LEDs are weak up there.

## Accuracy

| Term                      | Contribution |
|---------------------------|--------------|
| 8 MHz HSE crystal         | ±30 ppm → 0.003 %, negligible |
| Sample grid               | ±2 µs worst case between raw samples: 0.8 % at 1/4000, 0.2 % at 1/1000. This is the *upper bound without interpolation*, not the operating error |
| Interpolation residual    | Crossing jitter ≈ 1 LSB of voltage noise ÷ edge slope. With ~20 mV/µs optical edges that is tens of ns per edge. The grid itself has no jitter — conversions start on timer events |
| Phototransistor t_r/t_f   | 5…9 µs depending on the bin (datasheet: I_C = 1 mA, V_CC = 5 V, R_L = 1 kΩ). Both edges are delayed about equally, so it largely cancels in the width; the residual comes from rise/fall asymmetry and from saturation recovery |
| Optical edge shape        | **Dominant at fast speeds.** At 1/4000 the slit is barely wider than the chip, so the light at the sensor is a trapezoid with edges tens of µs wide. Measuring between equal-fraction crossings cancels this — and the auto-midpoint threshold *is* an equal-fraction crossing, recomputed per pulse, instead of a trimpot set by eye |
| Plateau estimation        | The midpoint is only as good as its two plateaus. A light level that changes *during* a pulse or between pulses (notably xenon flash decay) biases `V_thr` and the width. Reproducible only at a steady source |

Bottom line: ~0.1 % class from 1/30 to 1/2 with a repeatable setup; expect a
few percent at 1/4000, where the geometry of the shutter slit dominates and
the sampling chain contributes a few × 0.1 %. Keep the sign of the error in
mind: a saturated front end biases every fast reading long — but it now also
prints `clipped`, which is the point of sampling the voltage instead of merely
comparing it.

### What it does not measure

- **One point in the frame.** A focal-plane shutter exposes different points at
  different times; the sensor sees the light at its own position, so you get the
  local exposure time, not curtain-travel uniformity across the frame.
- Flash sync timing, and anything about the lens aperture.
- Absolute exposure: a midpoint between two plateau estimates is not a
  photometric standard. Use the tester comparatively (same setup, compare
  against nominal or another body).

## Build, flash, run

Requires PlatformIO (`pipx install platformio`, or the VS Code extension).

```sh
pio run                 # build the default env, blackpill_f103c8
pio run -e blackpill_f103c8_128   # build a 128 KiB clone
pio run -t upload       # flash over ST-Link (SWD: 3V3, GND, PA13 = SWDIO, PA14 = SWCLK)
pio device monitor      # read the reports from /dev/ttyACM0
```

No ST-Link and no stm32duino bootloader? The F103's ROM bootloader needs no
extra hardware, but not over USB: wire a USB-serial adapter (3V3, GND, TX→PA10,
RX→PA9), set `upload_protocol = serial` in `platformio.ini`, put the BOOT0
jumper at 1, reset, upload, then return BOOT0 to 0 and reset. `dfu` is *not* in
this board's upload protocol list: USB upload requires the stm32duino (Maple)
bootloader flashed at 0x08002000 first, and even then it enumerates as
`1EAF:0003`, not as the ST ROM DFU device. Full command matrix and design
invariants: [`AGENTS.md`](AGENTS.md).

### Board notes worth knowing before you blame the firmware

The STM32F103 has no internal D+ pull-up, so the board must fit **1.5 kΩ from
PA12 to 3V3**. The Black Pill populates it correctly, so unlike the Blue Pill
there is no "measure `R10` before debugging" trap here; a dead CDC port means
firmware or cabling. Regulator is an ME6211 (3.3 V, 180 mA), which is ample:
the front end draws ~1 mA. `blackpill_f103c8_128` in the board list if your
board has the 128 KiB part. The user LED is **PB12** on the Black Pill (PC13 on
the Blue Pill) — active-low, sinks through the pin, so `LOW` lights it.
