# AGENTS.md — shuttercheck firmware notes

Repo: camera shutter speed tester on a Black Pill (STM32F103C8T6). User-facing
docs live in `README.md`; this file holds the build/flash commands, the
invariants the firmware must not break, and the facts already verified against
upstream sources so nobody has to re-derive them.

The analog front end is a phototransistor emitter node sampled **directly by
the MCU's ADC1** on a TIM3-triggered grid. There is no external comparator;
edge times are interpolated from the sample stream. The invariants below exist
because that move trades a 125 ns hardware latch for a 2 µs sample grid plus
arithmetic — everything else is exactly as conservative as before.

## Layout intent

- `src/main.cpp` — Arduino `setup()`/`loop()`. Wiring, report formatting. No
  measurement arithmetic beyond unit conversion.
- The capture path is bare-register TIM3 (sample clock, TRGO) + ADC1 +
  DMA1_Channel1 code plus the crossing-scan in the DMA half/transfer-complete
  handlers. Keep it in a separate translation unit from reporting so the scan
  timing stays obvious.
- Not yet created: `src/` does not exist, so `pio run` currently reports
  *nothing to build*. Do not add a placeholder sketch to silence that.

## Commands

```sh
pio run                      # build
pio run -t upload            # flash (default: ST-Link over SWD)
pio run -t clean
pio device monitor           # read reports on the USB CDC port
pio run -t upload -e blackpill_f103c8_128   # 128 KiB clone
```

- PlatformIO is **not installed in this development environment** (`pio` is
  absent, `~/.platformio` does not exist). Build/upload verification must run
  where PlatformIO is installed; do not claim a build was verified from here.
- SWD wiring: 3V3, GND, PA13 (SWDIO), PA14 (SWCLK), NRST. `debug_tool`
  defaults to `stlink` from the board definition.
- Without an ST-Link: `upload_protocol = serial` with a USB-serial adapter on
  PA9/PA10, jumper BOOT0 = 1, reset, upload via the F103 ROM bootloader, then
  BOOT0 = 0 + reset. `dfu` is NOT usable: `blackpill_f103c8`'s
  `upload.protocols` is `jlink, cmsis-dap, stlink, blackmagic, serial` — no
  `dfu`, no `mbed`. On the F103 the platform's `dfu` branch does not even use
  the ROM bootloader: it runs `maple_upload` against the stm32duino bootloader
  (`1EAF:0003`, flashed at 0x08002000), with `upload.boot_version` defaulting
  to 2.
- 128 KiB clones: `board = blackpill_f103c8_128`. Only the linker limit
  differs; `LED_BUILTIN` is PB12 for both (see below).

## Verified environment facts

Board/platform (from `platform-ststm32` board JSONs, `boards/blackpill_f103c8.json`
and `blackpill_f103c8_128.json`): `mcu = stm32f103c8t6`, `cpu = cortex-m3`,
`f_cpu = 72000000L`, `extra_flags = -DSTM32F1 -DSTM32F103xB`, variant
`variant_PILL_F103Cx.h`, `maximum_size` 65536 (131072 for the `_128` board),
RAM 20480, `hwids = 0x1EAF:0x0003, 0x1EAF:0x0004`, `upload.protocol = stlink`
with `protocols = jlink, cmsis-dap, stlink, blackmagic, serial`. The Black Pill
JSON differs from the Blue Pill one only in `name` and in that shorter protocol
list, so every clock/capture fact below is unchanged from the Blue Pill design.

USB CDC recipe (PlatformIO builder `framework-arduinoststm32`,
`tools/platformio/platformio-build.py` ~L86-125): with
`-D PIO_FRAMEWORK_ARDUINO_ENABLE_CDC` in `build_flags`, the builder defines
`USBD_USE_CDC`, `USBCON`, `USB_VID`/`USB_PID` from the board's first `hwids`
entry (`0x1EAF:0x0003`), compiles `libraries/USBDevice`, and adds
`HAL_PCD_MODULE_ENABLED`. `cores/arduino/WSerial.h` then maps
`Serial` → `SerialUSB` under `#if defined(USBCON) && defined(USBD_USE_CDC)`.
Defining `USBD_USE_CDC` by hand instead of using the PIO flag loses `USBCON`,
VID/PID and the USBDevice sources.

Clock tree (`variants/STM32F1xx/F103C8T_F103CB(T-U)/variant_PILL_F103Cx.cpp`,
`SystemClock_Config`): `SYSCLK = HSE 8 MHz × 9 = 72 MHz`, AHB /1, APB1 /2,
APB2 /1, ADC = PCLK2/6 = 12 MHz, USB = PLL ÷ 1.5 = 48 MHz. Therefore
TIM3 (APB1, ×2) counts at **72 MHz** and the ADC runs at **12 MHz** — the two
numbers the sampling budget below is computed from; `F_CPU` is `72000000L`.

LED: the same variant defines `LED_GREEN = PC13` only when
`ARDUINO_BLUEPILL_F103C8` or `ARDUINO_BLUEPILL_F103CB` is defined; otherwise
`LED_GREEN = PB12` (the comment in `variant_PILL_F103Cx.h` says "LED
Blackpill"), and `LED_BUILTIN = LED_GREEN`. The builder defines
`ARDUINO_<BOARDID>` upper-cased (`platformio-build.py` L210-254), so
`blackpill_f103c8` → `ARDUINO_BLACKPILL_F103C8`, which is neither Blue Pill
macro, i.e. `LED_BUILTIN = PB12`. Do not rely on the board JSON `extra_flags`
for this, and do not set `ARDUINO_BLUEPILL_*` to "get the LED back". PB12 is
active-low and sinks the LED through the pin (`variant_PILL_F103Cx.h` comments
it as the Blackpill LED), so `LOW` lights it; it is a normal GPIO, unlike
PC13 (RTC/TAMPER, 3 mA-ish and slow edges) — never drive a heavy load either
way.

USB hardware: the F103 has **no internal D+ pull-up**; the board must fit
1.5 kΩ from PA12 to 3V3. The Black Pill fits it, so the Blue Pill clone defect
(`R10` 10 kΩ / 4.7 kΩ, flaky or impossible enumeration) does not apply here.

## Design invariants

1. **Timer-triggered sampling only.** Conversions are started by TIM3 TRGO
   (update events, ADC1 `EXTSEL = TIM3 TRGO`) and landed by DMA1_Channel1 in a
   circular buffer. The sample clock is `PSC = 143`, `ARR = 0` at 72 MHz →
   exactly 2.000 µs per sample, and the grid has no software jitter. Never
   measure an edge by code execution time, never poll the pin, never run the
   ADC on a software loop or `analogRead()`.
2. **One fixed range, no state machine.** 500 kS/s covers the rated
   1/4000 … 6 s with ≥125 samples on the shortest pulse. Do not re-introduce
   prescaler ranges; do not add an `overflow` status — nothing overflows, and
   rejection is defined by invariants 4–6 only.
3. **ADC configuration is fixed and calibrated.** ADCCLK = 12 MHz (already the
   variant's clock setup), `SMP = 7.5` cycles → 1.67 µs conversion, inside the
   2 µs trigger period with margin. 7.5 cycles is the floor for the
   datasheet's sampling-time-vs-external-impedance spec with the ≈1.1 kΩ front
   end (`R_E` 1 kΩ + 100 Ω series); do not shorten it. Run the power-on ADC
   calibration (`RSTCLB`, then `CLB`, wait `EOC`) after power-up and after
   wake-up before enabling conversions — an uncalibrated F103 ADC has
   meaningless absolute accuracy.
4. **Threshold: auto midpoint with hysteresis.** `V_thr` = midpoint of the
   tracked dark and bright plateau estimates, recomputed per pulse; plateaus
   are estimated only from samples that are neither part of a crossing pair
   nor inside the hysteresis band. Band = max(8 LSB, span/16); a pulse is not
   re-armed until the signal has fallen to the low band edge. Noise must never
   be able to fake an edge, and no trimpot-equivalent knob may reappear.
5. **Edge bookkeeping:** a sample is valid only if exactly one rising and one
   falling crossing belong to the same above-threshold excursion. A falling
   crossing whose rising edge was never seen (power-up during an exposure) is
   `stale` and must be dropped. Unlike the old capture design, a pulse can
   never "complete before the ISR ran" — DMA is always sampling — so no
   recovery special case for that.
6. **Conservative failure.** Rejection paths: `clipped` — bright plateau
   within 32 LSB of full scale, i.e. saturated front end; `weak` — bright−dark
   span below 48 LSB (≈40 mV), i.e. the midpoint would be noise. Every
   rejection discards the measurement; the firmware never prints a value it
   has not verified. Losing a sample is acceptable, printing a wrong one is
   not.
7. **No interrupts-off stretches in the capture path.** The scan of one chunk
   must complete well inside the chunk period: with a 2×512-sample buffer the
   budget is ~1.024 ms at 500 kS/s — leave it as a hard constraint on chunk
   size and scan cost. `SerialUSB.write()` blocks when the host is slow, so
   report from `loop()` only, never from an ISR, and keep a small result FIFO
   so a slow host cannot gate the scan. (There is no serial-silent
   measurement window: hardware sampling is immune to USB activity.)
8. **Integer math only** in the measurement path. Crossing time is
   `t_ns = (n−1)·2000 + (V_thr − v_{n−1})·2000 / (v_n − v_{n−1})` with integer
   division; widths are carried in nanoseconds in a **64-bit** accumulator
   (a 6 s Bulb exceeds 2³² ns — do not "fix" this with 32-bit to save memory).
   No floats, no `millis()`/`micros()` in the measurement.
9. **Polarity:** dark → low voltage at PA1, light → high; rising crossing =
   shutter opens, falling = shutter closes. Symptoms of a reversed input are a
   `stale` flood or a width that equals the dark interval — check the wiring,
   not the algorithm. A width that grows with the lamp level is front-end
   saturation, which the firmware now *also* reports as `clipped`: fix the
   light level, not the code.
10. **Feedback:** one short flash on **PB12** (`LED_BUILTIN` on the Black
    Pill; PC13 on a Blue Pill) per accepted sample; nothing on rejection.

## Front end (analog)

Sensor is an **SFH 309 FA** Si NPN phototransistor: collector to 3V3, emitter
via `R_E` = 1 kΩ to GND (node A), node A through **100 Ω series** into
**PA1 = ADC1_IN1**. The emitter follower is non-inverting, so the firmware's
polarity convention holds as long as the sensor is wired collector-to-3V3.
There is no comparator, no reference divider, and no adjustable threshold
anywhere in the analog path — if you find yourself adding one, you are
reverting the design decision, not fixing a bug.

- Datasheet numbers that drive the error budget: I_PCE 400…5000 µA over the
  bins at λ = 950 nm, E_e = 0.5 mW/cm², V_CE = 5 V; t_r/t_f 5…9 µs by bin at
  I_C = 1 mA, V_CC = 5 V, R_L = 1 kΩ; V_CEsat 200 mV; dark current 1 nA typ;
  spectral range 730…1120 nm, peak 900 nm; half angle ±12°.
- Keep the emitter plateau at 0.3…1 V. Saturation (node near 3V3) stores base
  charge and delays turn-off, which shows up as fast exposures reading *long*
  — and as `clipped`, because the ADC sees the top of the range. That is a
  light-level problem, not something the firmware may compensate for or clamp.
- The 100 Ω series resistor is a requirement, not a nicety: it isolates the
  ADC's sampling-charge kick and limits fault current into a pin that is not
  5 V tolerant in analog mode. Keep total source impedance ≤ ~1.2 kΩ so the
  7.5-cycle sample time remains inside the datasheet accuracy spec.
- If a pin capacitor is fitted for HF noise it must be **≤ 1 nF**. With
  `R_E` = 1 kΩ, a habitual 100 nF bypass makes a 100 µs RC that smears a
  250 µs pulse into mush. 1 nF adds ~1 µs symmetrically to both edges.
- Below ~40 mV of plateau span (`weak`) the midpoint is noise: 1 LSB = 0.8 mV
  and real ENOB is ~9–10 bits. Dim-light behaviour is a light-level problem
  too; do not lower the rejection threshold to "make it work".
- At 1/4000 the dominant error is optical (slit width vs. chip size), not the
  sampling chain. Do not chase it in code.

## Conflicting peripherals

`variant_PILL_F103Cx.h` reserves `TIMER_TONE = TIM3` and `TIMER_SERVO = TIM2`,
`SERIAL_UART_INSTANCE = 1` (PA9/PA10), SPI on PA5-PA7, I2C on PB6/PB7.

- **TIM3 is the sample clock.** `tone()` reconfigures TIM3's PSC/ARR and would
  silently destroy the 2 µs grid — never call it. Same for anything else that
  touches TIM3.
- **ADC1 and PA1 are the capture input.** Never call `analogRead()` (the
  framework reconfigures the ADC for polled single conversion and drops the
  TRGO trigger), and never reconfigure ADC1/DMA1_Channel1 from sketch code.
  PA1 carries only the sensor path; it must not also be used by anything else.
- TIM2 (`TIMER_SERVO`) stays unused: the Servo library is not linked.
- PA9/PA10 stay free: use them if a UART debug channel is ever wanted.
- PA11/PA12 are the USB pins; never reuse them for the front end.
- `LED_BUILTIN` = PB12 on this board. PB12 is not the bootloader strap
  (BOOT1 = PB2) and is not used by any peripheral above, so it is free for the
  feedback flash; only the LED may hang off it.

## Verification policy

Firmware behaviour is verified **on hardware**, not by a host test suite:

```sh
pio run -t upload && pio device monitor
# fire the shutter, check the printed number against the dial setting
```

For a known reference without a camera, inject a 3.3 V pulse into node A (or
into PA1 **through ≥100 Ω series**) from a function generator or a second MCU:
verify 1/1000 and, if the source can do it, ~250 µs against the generator's
own width. Exercise every rejection path deliberately: a falling-only pulse →
`stale`; a long HIGH plateau near 3V3 → `clipped`; a low-amplitude pulse
(< 40 mV span) → `weak`. Then sanity-check the interpolation: the reported
width of a fixed-length pulse should stay stable as you sweep its length
across ±2 µs — if it locks to multiples of the sample grid, something is
reporting sample indices instead of interpolated times.

If the crossing scan, threshold/plateau estimation or interpolation grows
host-testable logic, add a PlatformIO `native` env and test it there rather
than on the board.

## Gotchas

- `USBD_USE_CDC` without the PIO flag: see above.
- Forgetting the ADC calibration sequence: conversions run, values look
  plausible, absolute accuracy is garbage. See invariant 3.
- 100 nF on PA1: see the front-end note; this bug *looks* like a slow shutter.
- `Serial.flush()` on this core waits for the host to drain the CDC buffer; it
  is not a no-op like on AVR.
- The F103 ROM bootloader needs the HSE crystal and PA11/PA12, and after a
  `serial` upload the BOOT0 jumper must go back to 0 or the firmware never
  starts. It is reachable only over USART1 (PA9/PA10), never over USB: the
  straight-through USB cable goes to the F103's own USB device controller, and
  there is no ROM DFU there. Uploading over USB additionally needs the
  stm32duino bootloader in flash (see the `dfu` note under Commands).
- `#include "Wire.h"`/`SPI.h` reserve PB6/PB7 and PA5-PA7; avoid enabling them.
