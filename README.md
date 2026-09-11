# shuttercheck

Camera shutter speed tester. An SFH 309 FA phototransistor watches the light
that passes through the shutter, a comparator turns its output into a clean
digital pulse, and a Blue Pill (STM32F103C8T6) timestamps both edges in hardware
with a timer input-capture unit. The measured exposure is printed over USB CDC;
any serial terminal reads it.

> **Status: scaffolding only.** This repo currently contains `README.md`,
> `AGENTS.md`, `platformio.ini` and `.gitignore`. The firmware is not written
> yet, so `pio run` reports *nothing to build* until `src/` exists.
> `AGENTS.md` holds the build/flash commands and the design invariants the
> firmware has to satisfy.

## How it measures

1. **SFH 309 FA** (Si NPN phototransistor, T1 3 mm radial): collector to 3V3,
   emitter to the load resistor `R_E` to GND. The photocurrent is multiplied by
   the transistor's current gain, so the emitter node swings by hundreds of mV
   under normal test light — no extra amplifier stage.
2. An LM393 compares the emitter node against a trimpot threshold. A feedback
   resistor adds a few mV of hysteresis so a slowly crossing shutter edge
   produces one clean transition instead of a burst.
3. The comparator's open-collector output (pulled up to 3V3) drives
   **PA6 = TIM3_CH1**.
4. TIM3 runs on the 72 MHz APB1 timer clock in **PWM-input slave mode**:
   the rising edge latches CCR1 *and* resets the counter, the falling edge
   latches CCR2. The pulse width is `CCR2` timer ticks, latched by hardware —
   edge timing does not depend on interrupt latency at all.
5. Auto-ranging: a pulse that overflows 16 bits is rejected, the prescaler
   steps up, and you fire the shutter again. A valid measurement below half of
   the current range steps back down.

| Range | Prescaler | Tick   | Full scale | Typical speeds     |
|-------|-----------|--------|------------|--------------------|
| F     | PSC = 8   | 125 ns | 8.19 ms    | 1/8000 … 1/125     |
| M     | PSC = 719 | 10 µs  | 655 ms     | 1/125 … 1/2        |
| S     | PSC = 7199| 100 µs | 6.55 s     | 1/2 … 6 s, Bulb    |

Polarity convention: dark → comparator output LOW, light → HIGH. Rising edge is
the shutter opening, falling edge the shutter closing. The phototransistor is
an emitter follower, so the front end is non-inverting: more light, higher
emitter voltage, high comparator output.

### Output

One line per capture on the USB CDC port: nanoseconds, then a status.

```
8123000 ok
0 overflow
0 stale
```

| Status     | Meaning                                                        |
|------------|----------------------------------------------------------------|
| `ok`       | Both edges captured after the counter was reset; count is exact |
| `overflow` | Pulse exceeded this range's full scale; range escalated         |
| `stale`    | A falling edge arrived without a rising edge of its own pulse   |

`8123000 ns` = 8.123 ms ≈ 1/123 s. Feed it to any serial terminal
(`pio device monitor`); no host-side software is required.

## Wiring

The whole front end runs from **3V3** — no 5 V anywhere near the MCU pin.

| From                        | To                                | Notes |
|-----------------------------|-----------------------------------|-------|
| SFH 309 FA collector        | 3V3                               | Long lead |
| SFH 309 FA emitter          | node A                            | Pin 1 / short lead. Aim the lens at the shutter; half angle is only ±12° |
| `R_E` 1 kΩ                  | node A → GND                      | Sets amplitude *and* speed. 1 kΩ is the datasheet's t_r/t_f condition; 100 Ω is faster but needs more light |
| Trimpot 10 kΩ               | 3V3 → 10 kΩ → trimpot → GND, wiper → LM393 pin 2 | Series resistor makes the wiper span 0…1.65 V, i.e. all of the LM393's usable 0…1.8 V window at Vcc = 3.3 V |
| LM393 pin 3 (+IN A)         | node A                            | |
| LM393 pin 1 (OUT A)         | 10 kΩ pull-up → 3V3, and to PA6   | Open collector; never pull it above 3V3 |
| 1 MΩ feedback               | LM393 pin 1 → node A              | ≈3 mV of hysteresis with `R_E` = 1 kΩ. Increase if the output chatters in dim light, decrease if the threshold walks |
| LM393 pin 8 / pin 4         | 3V3 / GND                         | 100 nF directly across pins 8 and 4 |
| Spare half (5, 6, 7)        | pin 5 → GND, pin 6 → 3V3          | Defines its output; leave pin 7 open |
| Blue Pill                   | USB → PC                          | Board 3V3/GND feed the front end |

Front-end sizing that matters:

- **Keep the emitter plateau around 0.3…1 V.** At 3 V the phototransistor is
  saturated: `V_CEsat` is 200 mV and the stored base charge delays turn-off, so
  the closing edge reads late and fast exposures look *long*. Dim the source
  (lens aperture, distance, diffuser) until the node sits in range. Below
  ~10 mV the LM393's offset (2 mV typ, 5 mV max) starts to matter.
- **Light source spectra.** Sensitivity peaks at 900 nm (range 730…1120 nm):
  daylight, tungsten and xenon flash are ideal, white LEDs are weak up there.
- The LM393's own propagation delay (~1.3 µs typ, worse for small overdrive) is
  negligible next to the phototransistor's rise time.

## Accuracy

| Term                      | Contribution |
|---------------------------|--------------|
| 8 MHz HSE crystal         | ±30 ppm → 0.003 %, negligible |
| Timer quantization        | ±1 tick: 0.1 % at 1/8000 (F), 0.0016 % at 1/125 (F), 0.12 % at 1/125 (M), 0.1 % at 0.1 s (S) |
| Phototransistor t_r/t_f   | 5…9 µs depending on the bin (datasheet: I_C = 1 mA, V_CC = 5 V, R_L = 1 kΩ). Both edges are delayed about equally, so it largely cancels in the width; the residual comes from rise/fall asymmetry and from saturation recovery |
| Optical edge shape        | **Dominant at fast speeds.** At 1/8000 the slit is barely wider than the chip, so the light at the sensor is a trapezoid with edges tens of µs wide. Measuring between equal-fraction crossings cancels this; a threshold or light-level change does not |
| Threshold placement      | Not an error in the timing chain, but the number is defined by where the light crosses the threshold. Reproducible only at the same threshold and light level |

Bottom line: ~0.1 % class from 1/30 to 1/2 with a repeatable setup; expect a few
percent at 1/4000…1/8000, where the geometry of the shutter slit dominates and
the MCU contributes nothing measurable. Keep the sign of the error in mind: a
saturated front end biases every fast reading long.

### What it does not measure

- **One point in the frame.** A focal-plane shutter exposes different points at
  different times; the sensor sees the light at its own position, so you get the
  local exposure time, not curtain-travel uniformity across the frame.
- Flash sync timing, and anything about the lens aperture.
- Absolute exposure: a comparator threshold is not a photometric standard. Use
  the tester comparatively (same setup, compare against nominal or another body).

## Build, flash, run

Requires PlatformIO (`pipx install platformio`, or the VS Code extension).

```sh
pio run                 # build (after src/ exists)
pio run -t upload       # flash over ST-Link (SWD: 3V3, GND, PA13 = SWDIO, PA14 = SWCLK)
pio device monitor      # read the reports from /dev/ttyACM0
```

No ST-Link? The F103's built-in ROM DFU bootloader needs no extra hardware: set
`upload_protocol = dfu` in `platformio.ini`, put the BOOT0 jumper at 1, reset,
upload, then return BOOT0 to 0 and reset. Full command matrix and design
invariants: [`AGENTS.md`](AGENTS.md).

### USB gotcha worth knowing before you blame the firmware

The STM32F103 has no internal D+ pull-up; the board must fit **1.5 kΩ from PA12
to 3V3**. Many Blue Pill clones populate `R10` with 10 kΩ (some 4.7 kΩ), which
makes enumeration slow, flaky or impossible. If the CDC port never appears,
measure that resistor before debugging the code — and note `bluepill_f103c8_128k`
in the board list if your clone has the 128 KiB part.
