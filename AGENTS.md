# AGENTS.md — shuttercheck firmware notes

Repo: camera shutter speed tester on a Black Pill (STM32F103C8T6). The design,
the wiring and the accuracy discussion live in `README.md`. This file holds the
rules only: workflow, layout, commands, platform facts, invariants.

## Workflow

- Every issue gets its own branch: create a branch from `main` for the issue,
  do the work there, and push to that branch — never to `main`.
- Open a Pull Request against `main` when the work is complete.
  Never merge into `main` directly.
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
  ADC, DMA) and the lightweight DMA callbacks that flag the newest finished
  buffer half. The crossing scan runs on that half from `loop()`, not in the
  DMA interrupt: the scan is a large O(samples) workload and keeping it in the
  ISR saturates the CPU and starves USB-CDC enumeration. Let `scan()` run at
  thread priority where the USB interrupt can preempt it.
- `src/scan.cpp` / `src/scan.h` — the crossing scan, exposure measurement and
  the result FIFO. `scan()` is a template on `scan::Polarity`: the signal
  direction is a compile-time constant, so it costs no cycle in the hot loop.
  The scan confirms a threshold crossing with two consecutive samples on the
  new side, but keeps the first crossing time for interpolation. Pure integer,
  stdint only, free of HAL and Arduino: the native unit tests compile it
  directly. Keep it separate from reporting so the scan timing stays obvious.
- `src/selfcheck.cpp` / `src/selfcheck.h` — the on-device self test (issue 8),
  compiled only into `ON_DEVICE_TEST` builds. It asks the HAL, capture path and
  pulsegen for peripheral state. It runs generated scan cases and reads no
  peripheral registers itself. Its last section sends a conditioning pulse to
  learn both plateaus, then measures one pulse per shutter duration.
- `src/pulsegen.cpp` / `src/pulsegen.h` — the test pulse generator (issue 18),
  compiled only into `ON_DEVICE_TEST` builds. TIM2_CH1 on PA0 emits one light
  pulse per arm call after 1 ms of dark lead-in; both edges are timer events,
  so the pulse has no software jitter. The pin idles dark (high), the same
  signal direction as the cascode front end. The selfcheck ADC section is the
  only caller.
- `src/debug.cpp` / `src/debug.h` — the debug mode (issue 12). It prints the
  state of the capture path and of the scan on the CDC port, one line every
  `kDebugChunkInterval` scanned chunks. The whole module sits behind
  `SHUTTERCHECK_DEBUG`, so a release build does not compile it, also not the
  tracking of the raw extremes in `scan.cpp`. It is firmware-only: it talks to
  Arduino and to `capture`, so the native build compiles it as an empty
  translation unit. It reads the path, not the registers, the same rule
  `selfcheck.cpp` follows. The print interval is a compile-time constant
  (`DEBUG_CHUNK_INTERVAL`); no run-time knob may reappear.
  The native env always defines `SHUTTERCHECK_DEBUG`, so `pio test -e native`
  covers the debug fields of the scan too. `printf` of this toolchain has no
  long long width: `%lld` prints `ld`, so `debug.cpp` formats 64-bit values
  itself. Never use `%lld` here.
- `test/scan_cases.h` — feeds one generated case through the scan and records
  what came out. Free of Unity and of the HAL, so `test/test_scan.cpp`
  (native, asserts with Unity) and `src/selfcheck.cpp` (on device, prints
  PASS/FAIL) share one feeder and one set of expectations. Each case runs in
  both polarities: the generated stream, then its mirror.
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
pio test -e native           # native scan unit tests (always with SHUTTERCHECK_DEBUG)
pio run -t upload            # flash (ST-Link over SWD)
pio run -t clean
pio device monitor           # read reports on the USB CDC port
pio run -t upload -e blackpill_f103c8_128   # 128 KiB clone
python scripts/on_device_test.py            # flash the self test, read its report
pio run -e debug            # build the debug env (issue 12: prints the capture and scan state)
pio run -e debug -t upload  # flash it
```

- `pio run -t upload` ends with an OpenOCD reset. The board runs the new
  firmware right after the flash. Without that reset OpenOCD leaves the core
  halted and nothing happens until you press reset.

## Platform facts (verified, do not re-derive)

- Board `stm32f103c8t6`, Cortex-M3, `F_CPU = 72000000L`, variant
  `variant_PILL_F103Cx`; 64 KiB flash and 20 KiB RAM on `blackpill_f103c8`,
  128 KiB on `blackpill_f103c8_128`.
- Clock tree: SYSCLK = HSE 8 MHz × 9 = 72 MHz, AHB /1, APB1 /2, APB2 /1. TIM3
  counts at 72 MHz, the ADC clock is 12 MHz (PCLK2/6), USB is 48 MHz.
- `LED_BUILTIN` is PB12 and active-low (`LOW` lights it). A Blue Pill macro
  would move it to PC13; do not define one.
- USB CDC needs `-D PIO_FRAMEWORK_ARDUINO_ENABLE_CDC` in `build_flags`. The
  framework builder turns it into `USBD_USE_CDC`, `USBCON`, `USB_VID`,
  `USB_PID` and `HAL_PCD_MODULE_ENABLED`, and it builds the USBDevice library.
  Do not define those by hand. The F103 has no internal D+ pull-up, so the
  board must fit 1.5 kΩ from PA12 to 3V3; the Black Pill does.
- The CDC transmit queue of the core holds 128 bytes
  (`CDC_TRANSMIT_QUEUE_BUFFER_PACKET_NUMBER` defaults to 2). A debug report is
  about 155 bytes, so `[env:debug]` raises it to 8 packets: a report longer
  than the queue blocks `loop()` until the USB moves the bytes, and a blocked
  loop loses buffer halves (the `lost` field).
- The build uses the pinned OpenOCD/ST-Link flow in `platformio.ini`. See
  `README.md` for the udev rule and for the serial bootloader path.

## Design invariants

1. Timer-triggered sampling only. Never time an edge in software, never poll
   the pin, never use `analogRead()`. On this TIM3, `ARR = 0` makes no periodic
   update event, so the trigger disappears and the ADC never converts.
2. One fixed 500 kS/s range; no range state machine. No prescaler ranges, no
   `overflow` status.
3. The ADC configuration is fixed, and the ADC is calibrated before the first
   conversion.
4. Threshold: the midpoint of the two tracked plateaus, with a hysteresis
   band. No trimpot-equivalent knob may reappear.
5. A sample is valid only if exactly one opening and one closing crossing
   belong to the same excursion. Confirm each crossing with one further sample
   on the new side. Cancel an unconfirmed excursion; keep the first crossing
   time for interpolation. A confirmed lone crossing is `stale`.
6. Conservative failure: the rejection paths are `clipped` and `weak`. Never
   print a value the firmware has not verified.
7. The scan of one buffer chunk must finish well inside that chunk's period.
   Report from `loop()` only, never from an ISR, through a small result FIFO.
8. Integer math only in the measurement path. Carry widths in nanoseconds in a
   64-bit accumulator; no floats, no `millis()`/`micros()`.
9. The signal direction at PA1 is a compile-time constant, `scan::Polarity`,
   and never a runtime value. The head in use is a cascode stage: it inverts,
   so dark → high voltage at PA1, light → low (V_bias). The shutter opens on
   the crossing that leaves the dark plateau.
10. One short flash on PB12 per accepted sample; nothing on rejection.

## Verification policy

Use CI and unit tests for hardware-independent work (compilation, arithmetic).
Validate hardware-dependent work _on hardware_: check for a suitable board, and
stop and ask how to proceed if none is found. Define the expected outcome
first, either in the issue or as a test.

`python scripts/on_device_test.py` is the hardware regression test: it flashes
the `on_device_test` environment, reads the report and exits non-zero on a
failure. It covers the clock tree, the capture start, the crossing scan and
the USB stack without a sensor, and the ADC pulse section (issue 18) covers
the ADC and DMA data path with a generated pulse. That section needs the test
bridge from `README.md` (PA0 -- 1 kOhm -- PA1 -- 10 kOhm -- GND, and 5 nF
from PA1 to GND; cascode output disconnected from PA1); without the bridge it
FAILs. It needs a board, so CI does not run it. The sensor front end still
needs a controlled optical input.

With no sensor you can still drive the whole capture to report path by hand.
Halt the core over SWD, write a known 512-sample pulse into the DMA buffer
(`capture::buffer` in `.bss`), zero the scan state and the result FIFO, set
`g_readyHalf` to the buffer half you wrote, clear the DMA channel enable so
nothing overwrites the data, then resume. The board prints the measurement for
those samples over USB CDC. Get the symbol addresses per build with
`arm-none-eabi-nm -S`: they move when the layout changes. Used on 2026-09-19 to
show `500000 ok` for a 500-sample pulse, the same value the native build of
`src/scan.cpp` gives.
