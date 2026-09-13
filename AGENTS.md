# AGENTS.md — shuttercheck firmware notes

Repo: camera shutter speed tester on a Black Pill (STM32F103C8T6). User-facing
docs live in `README.md`; this file holds the build/flash commands, the
invariants the firmware must not break, and the facts already verified against
upstream sources so nobody has to re-derive them.

## Workflow

- Every issue gets its own branch: create a branch from `main` for the issue,
  do the work there, and push to that branch — never to `main`.
- When the implementation is complete, open a Pull Request against `main`
  referencing the issue.
- Never merge into `main` directly; `main` only ever moves through PRs.
- When you write an issue for a new feature, and an implementation detail is
  not clear, stop and ask. Do not start the work before the answer.
- Write all text in ASD-STE100 Simplified Technical English: README,
  AGENTS.md, issues, PRs, commit messages, code comments, and chat replies.
  Keep sentences short. Use one meaning per sentence and per word. Prefer
  the active voice. Use the STE100 approved lexicon where it exists.

## Layout intent

- `src/main.cpp` — Arduino `setup()`/`loop()`. Wiring, report formatting. No
  measurement arithmetic beyond unit conversion.
- The capture path is TIM3 (sample clock, TRGO) + ADC1 + DMA1_Channel1 code
  plus the crossing-scan in the DMA half/transfer-complete handlers. Use the
  STM32 HAL where possible (handles, `HAL_ADC_Start_DMA`, `HAL_TIM` base,
  calibration, conversion callbacks). Go below the HAL only where the F1 HAL
  does not reach, and say why in a comment. Keep it in a separate translation
  unit from reporting so the scan timing stays obvious.

## Tools (prefer OMP-native over shell)

- Issues/PRs: read via `issue://<N>` / `pr://<N>`; use `gh` only for writes
  (create/comment/close) — the native URLs are read-only.
- Code intelligence: `lsp` (references/rename/code actions), `grep` tool, not
  shell `grep`/`rg`/`find`.
- Read files with `read` (supports line selectors); edit with `edit`.

## Commands

```sh
pio run                      # build
pio run -t upload            # flash (default: ST-Link over SWD)
pio run -t clean
pio device monitor           # read reports on the USB CDC port
pio run -t upload -e blackpill_f103c8_128   # 128 KiB clone
```

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
   circular buffer. The sample clock is `PSC = 71`, `ARR = 1` at 72 MHz →
   exactly 2.000 µs per sample, and the grid has no software jitter. Never
   set `ARR = 0`: this TIM3 then makes no periodic update event, TRGO gives no
   trigger, and the ADC never converts. This is measured on hardware, not
   theory. Never measure an edge by code execution time, never poll the pin,
   never run the ADC on a software loop or `analogRead()`.
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

## Verification policy

For simple functionality like successful compilation, properly implemeneted arithmetic and other hardware-independent functionality, use CI and unit tests.

Complex functionality that depends on hardware should be validated _on hardware_. Check the availability of a suitable board and stop and request how to proceed if none is detected.

always use a test-driven approach, where you define the expected outcome first, either verbally as part of an issue, or explicitly as a test case.

