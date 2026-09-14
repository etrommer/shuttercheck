# AGENTS.md — shuttercheck firmware notes

Repo: camera shutter speed tester on a Black Pill (STM32F103C8T6). The design,
the wiring and the accuracy discussion live in `README.md`. This file holds the
rules only: workflow, layout, commands, platform facts, invariants.

## Workflow

- Every issue gets its own branch: create a branch from `main` for the issue,
  do the work there, and push to that branch — never to `main`.
- Open a Pull Request against `main` when the work is complete, referencing
  the issue. Never merge into `main` directly.
- When you write an issue for a new feature, and an implementation detail is
  not clear, stop and ask. Do not start the work before the answer.
- Write all text in ASD-STE100 Simplified Technical English: README, AGENTS.md,
  issues, PRs, commit messages, code comments, and chat replies. Keep
  sentences short, use the active voice, use the approved lexicon.
- AGENTS.md are best practices and hints, not strict guidelines. If you believe
  that instructions are not in line with best practices, a clean, efficient
  implementation or the project's goals, stop and clarify. You as an agent may update
  the AGENTS.md file when appropriate.

## Layout intent

- `src/main.cpp` — Arduino `setup()`/`loop()`. Wiring, report formatting. No
  measurement arithmetic beyond unit conversion.
- `src/capture.cpp` / `src/capture.h` — the capture path (timer sample clock,
  ADC, DMA) and the DMA-callback hookup of the crossing scan.
- `src/scan.cpp` / `src/scan.h` — the crossing scan, exposure measurement and
  the result FIFO. Pure integer, stdint only, free of HAL and Arduino: the
  native unit tests (`pio test -e native`) compile it directly, so it must
  not pull in firmware-only headers. Keep it separate from reporting so the
  scan timing stays obvious.
- Use the STM32 HAL where possible. Go below the HAL only where the F1 HAL
  does not reach, and say why in a comment.

## Tools (prefer OMP-native over shell)

- Issues/PRs: read via `issue://<N>` / `pr://<N>`; use `gh` only for writes
  (create/comment/close) — the native URLs are read-only.
- Code intelligence: `lsp` (references/rename/code actions), `grep` tool, not
  shell `grep`/`rg`/`find`.
- Read files with `read` (supports line selectors); edit with `edit`.

## Commands

```sh
pio run                      # build
pio run -t upload            # flash (ST-Link over SWD)
pio run -t clean
pio device monitor           # read reports on the USB CDC port
pio run -t upload -e blackpill_f103c8_128   # 128 KiB clone
```

## Platform facts (verified, do not re-derive)

- Board `stm32f103c8t6`, Cortex-M3, `F_CPU = 72000000L`, variant
  `variant_PILL_F103Cx`; 64 KiB flash and 20 KiB RAM on `blackpill_f103c8`,
  128 KiB on `blackpill_f103c8_128`.
- Clock tree: SYSCLK = HSE 8 MHz × 9 = 72 MHz, AHB /1, APB1 /2, APB2 /1. TIM3
  counts at 72 MHz, the ADC clock is 12 MHz (PCLK2/6), USB is 48 MHz.
- `LED_BUILTIN` is PB12 and active-low (`LOW` lights it). A Blue Pill macro
  would move it to PC13; do not define one.
- USB CDC needs `-D PIO_FRAMEWORK_ARDUINO_ENABLE_CDC -D USBCON` in
  `build_flags`: the pinned framework does not set `USBCON` by itself. The
  F103 has no internal D+ pull-up, so the board must fit 1.5 kΩ from PA12 to
  3V3; the Black Pill does.
- The build uses the pinned OpenOCD/ST-Link flow in `platformio.ini`. See
  `README.md` for the udev rule and for the serial bootloader path.

## Design invariants

1. Timer-triggered sampling only. Never time an edge in software, never poll
   the pin, never use `analogRead()`. On this TIM3, `ARR = 0` makes no periodic
   update event, so the trigger disappears and the ADC never converts.
2. One fixed 500 kS/s range, no state machine. No prescaler ranges, no
   `overflow` status.
3. The ADC configuration is fixed, and the ADC is calibrated before the first
   conversion.
4. Threshold: the midpoint of the two tracked plateaus, with a hysteresis
   band. No trimpot-equivalent knob may reappear.
5. A sample is valid only if exactly one rising and one falling crossing
   belong to the same excursion. A lone falling crossing is `stale` and is
   dropped.
6. Conservative failure: the rejection paths are `clipped` and `weak`. Never
   print a value the firmware has not verified.
7. The scan of one buffer chunk must finish well inside that chunk's period.
   Report from `loop()` only, never from an ISR, through a small result FIFO.
8. Integer math only in the measurement path. Carry widths in nanoseconds in a
   64-bit accumulator; no floats, no `millis()`/`micros()`.
9. Dark → low voltage at PA1, light → high. Rising crossing = shutter opens,
   falling = shutter closes.
10. One short flash on PB12 per accepted sample; nothing on rejection.

## Verification policy

Use CI and unit tests for hardware-independent work (compilation, arithmetic).
Validate hardware-dependent work _on hardware_: check for a suitable board, and
stop and ask how to proceed if none is found. Define the expected outcome
first, either in the issue or as a test.
