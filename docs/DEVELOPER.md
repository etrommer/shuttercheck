# Developer reference

This guide describes the measurement path and the debug build. For user setup, see the [README](../README.md). For test commands and expected results, see [TESTING.md](TESTING.md).

## Capture and scan path

The STM32F103 samples PA1, which is ADC1_IN1, on a fixed 500 kS/s grid. TIM3 starts one 12-bit ADC conversion every 2.000 µs. DMA writes each result to a circular buffer.

DMA interrupts only mark the newest completed buffer half. `loop()` scans that half. A half contains 512 samples and takes 1.024 ms to fill. Keep the O(samples) scan out of the interrupt handler. A long interrupt can starve USB CDC and lose samples.

The scan code in `src/scan.cpp` is integer-only and does not use Arduino or the HAL. It carries exposure width in nanoseconds with a 64-bit accumulator. `src/capture.cpp` selects `scan::Polarity::kDarkHigh` for the inverting cascode front end. The scan is a template on polarity, so the direction is fixed at compile time. The native tests also run each generated case with the opposite polarity.

There is one fixed sample rate. Do not add timer ranges or overflow state. At 1/4000 s, the pulse contains about 125 samples. A 6 s exposure is a longer stream. The measurement path uses integer math; do not add floating point or `millis()`/`micros()` timing.

## Crossing algorithm

The cascode front end inverts the signal. With the shutter closed, the dark plateau is high. With the shutter open, the light pulls PA1 down toward the bias voltage. The shutter opens on a falling crossing and closes on a rising crossing. A direct-coupled front end has the opposite polarity.

### Plateaus and threshold

The first sample after reset seeds the dark plateau. The bright estimate starts 16 LSB away, on the side where the lit plateau should be. The first pulse moves it toward the measured lit level.

The scan updates a plateau with an integer exponential moving average:

```text
value = value + (sample - value) / 64
```

The division truncates. The scan uses the previous sample for an update. It excludes samples in the hysteresis band and samples in a crossing pair. A sample below the low band edge updates the low plateau; a sample above the high band edge updates the high plateau.

The threshold is the midpoint of the two estimates:

```text
threshold = (dark + bright) / 2
```

The hysteresis band width is `max(8 LSB, plateau span / 16)`. Its edges are `threshold ± band width / 2`. The band rejects small noise steps and keeps transition samples out of the plateau estimates.

### Opening, closing, and interpolation

An opening can start after a sample returns to the dark-side band edge. The next crossing must go through the threshold toward the light side. The scan stores the first crossing time as a candidate. A second consecutive sample on the light side confirms it. If the signal returns before confirmation, the scan cancels the candidate.

When a pulse opens, the scan locks the threshold. The plateau estimates can still move, but both edges use the same threshold. This avoids a drifting threshold, such as one caused by a decaying xenon flash, moving the measured width.

A closing crossing goes back through the locked threshold to the dark side. The scan stores its first crossing time. The next sample must remain on the dark side to confirm the close. Otherwise, the scan cancels that candidate and keeps the pulse open.

For the two samples around a crossing, the scan interpolates the crossing time:

```text
t_cross = (n - 1) * 2 µs
        + (threshold - sample[n - 1]) / (sample[n] - sample[n - 1]) * 2 µs
```

The exposure is the confirmed closing time minus the opening time. A result is valid only when exactly one opening and one closing crossing belong to the same excursion.

A confirmed crossing back to dark when no pulse is open is `stale`: it has no matching opening. An unconfirmed crossing is cancelled and does not create a result.

### Rejection rules

- `clipped`: a raw sample on the rail-side plateau reaches 4063 or more. This is 32 LSB below the 4095 full-scale value. The rail flag stays set for the excursion and clears when the pulse closes. With the cascode head, the rail-side plateau is dark. The scan checks raw samples because an integer moving average can stop about 64 LSB below a constant input.
- `weak`: the distance between the plateau estimates is less than 48 LSB, about 40 mV. The midpoint is then too close to the effective ADC noise floor.

A raw sample may not reach or exceed the ADC supply. For the F103, keep PA1 below 3.3 V + 0.3 V.

### Scan fast path

When no pulse is open, a run of samples can be skipped if every sample stays strictly inside the band or equals its plateau. Such a run cannot start an edge and cannot move an estimate. The normal scan still processes all other samples in order.

## Accuracy model

The stated operating range is 1/4000 s to 6 s. The raw sample grid alone has a worst-case width error of 2 µs: 0.8% at 1/4000 s and 0.2% at 1/1000 s. These are bounds without interpolation, not the expected operating error.

Representative sample counts and interpolation estimates:

| Speed | Exposure | Samples | Raw-grid bound | With interpolation |
|---|---:|---:|---:|---:|
| 1/4000 | 250 µs | 125 | ±0.8% | ≈0.05%, noise-limited |
| 1/1000 | 1 ms | 500 | ±0.2% | ≈0.02% |
| 1/30 | 33 ms | 16,700 | ±0.006% | Threshold noise |

These interpolation estimates describe the scan, not total camera accuracy. Optical geometry and sensor response dominate fast exposures.

Accuracy contributors:

| Source | Effect |
|---|---|
| 8 MHz HSE crystal | ±30 ppm, or 0.003%; negligible here. |
| Timer and ADC sample grid | Hardware events have no software timing jitter. Linear interpolation estimates the crossing between samples. |
| Voltage noise | Crossing jitter is about one LSB divided by the edge slope. At an optical slope near 20 mV/µs, this is tens of nanoseconds per edge. |
| Phototransistor response | Rise and fall times are 5–9 µs for the datasheet conditions (`I_C = 1 mA`, `V_CC = 5 V`, `R_L = 1 kΩ`). Similar delay on both edges mostly cancels. Rise/fall asymmetry and saturation recovery remain. |
| Optical edge shape | This is the largest error at fast speeds. At 1/4000 s, the shutter slit is only a little wider than the sensor chip. The light edge can be tens of microseconds wide. Measuring both edges at the same fraction of the plateau swing reduces this error. |
| Plateau drift | A changing light level, especially a decaying xenon flash, moves the estimated midpoint and biases the width. Use a repeatable light source. |

Expect about 0.1% class from 1/30 s to 1/2 s with a repeatable setup. Expect a few percent at 1/4000 s; shutter geometry dominates. Saturation recovery can make fast readings long. The `clipped` status reports a front end near the ADC rail.

## Debug build

The debug build reports capture and scan state over the CDC port. Release builds do not include the debug module or raw-extreme tracking.

```sh
pio run -e debug -t upload
pio device monitor
```

The build prints one report every 16 scanned chunks, about 61 reports per second. It prints a legend before the first report. Levels are in 12-bit ADC LSB. `DEBUG_CHUNK_INTERVAL` is a compile-time constant; set it in the debug build flags to change the interval. There is no runtime setting.

One report is about 155 bytes. The normal CDC queue holds 128 bytes. The debug environment raises the queue to eight 64-byte packets so a report does not block `loop()` and cause lost buffer halves.

Example:

```text
# shuttercheck debug: one report per 16 scanned chunks; levels in LSB
#48 min=2041 max=3903 lo=2038 hi=3904 dark=3896 bright=2049 span=1847 thr=2972 band=230 rail=0 pulse=0 lost=0 ok=3 stale=0 clip=0 weak=0 last=ok ns=500000
```

| Field | Meaning |
|---|---|
| `n` | Reported chunk number since start. |
| `min`, `max` | Lowest and highest raw sample in this chunk. |
| `lo`, `hi` | Lowest and highest raw sample across all chunks since start. |
| `dark`, `bright` | Current plateau estimates. |
| `span` | Positive distance between the two plateaus. |
| `thr` | Midpoint threshold. |
| `band` | Full hysteresis band width. |
| `rail` | `1` while the rail latch is set for the current excursion. |
| `pulse` | `1` while a pulse is open or a close is being confirmed. |
| `lost` | Buffer halves overwritten before the scan processed them. Keep this at `0`. |
| `ok`, `stale`, `clip`, `weak` | Accepted pulses and each rejection count since start. |
| `last` | Status of the last result, or `none`. |
| `ns` | Exposure of the last result, or `0`. |

A rising `lost` value means the scan or printing cannot keep up. `min`/`max` show recent levels; `lo`/`hi` show whether a level moved between reports. A `span` below 48 LSB produces `weak`. A raw level within 32 LSB of full scale produces `clipped`.

For a build-time report interval of 32 chunks, add `-D DEBUG_CHUNK_INTERVAL=32` to the debug environment's build flags. Longer intervals reduce USB output.

## Schematic source

Run `uv run docs/generate_schematic.py` to regenerate both circuit drawings.
