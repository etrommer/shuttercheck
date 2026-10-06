# AGENTS.md — shuttercheck firmware notes

Repo: camera shutter speed tester on a Black Pill (STM32F103C8T6).

## Workflow

- Every large work item starts from an issue. If there isn't one, create it.
  If there is one that is not specific enough, clarify and post a detailed plan
  as a comment
- For existing issues, always read the entire discussion thread
- Create a branch from `main` for an issue,
  do the work there, and push to that branch — never to `main`.
- Open a Pull Request against `main` when the work is complete.
  Reference the issue first, then add the description, e.g.
  "#5: Fix ADC frontend"
- When you write an issue for a new feature or are implementing an issue
  and an implementation detail is  not clear, stop and ask.
  Use best judgement where appropriate, but do not start implementing when there
  are significant design tradeoffs and decisions to be considered.
- Write all text in ASD-STE100 Simplified Technical English: README, AGENTS.md,
  issues, PRs, commit messages, code comments, and chat replies. Keep
  sentences short, use the active voice, use the approved lexicon.
- AGENTS.md are best practices and hints, not strict guidelines. If you believe
  that instructions are not in line with best practices, a clean, efficient
  implementation or the project's goals, stop and clarify. You as an agent may update
  the AGENTS.md file when appropriate.

## Commands

- `pio run -t upload` ends with an OpenOCD reset. The board runs the new
  firmware right after the flash. Without that reset OpenOCD leaves the core
  halted and nothing happens until you press reset.

## Scan implementation

`src/scan.cpp` is integer-only and has no HAL or Arduino dependency. Native
tests compile it directly.

Keep `scan()` state local for each chunk. Use `Band` for the threshold and
hysteresis edges. Keep signal direction at compile time. `detectCrossing()`
handles opening and stale edges. The open-pulse path handles closing edges with
its locked threshold. Compute absolute sample time only when an edge starts.
Keep sample order and state transitions unchanged in a refactor. Run the native
tests after scan changes.

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
FAILs.

With no sensor you can still drive the whole capture to report path by hand.
Halt the core over SWD, write a known 512-sample pulse into the DMA buffer
(`capture::buffer` in `.bss`), zero the scan state and the result FIFO, set
`g_readyHalf` to the buffer half you wrote, clear the DMA channel enable so
nothing overwrites the data, then resume. The board prints the measurement for
those samples over USB CDC. Get the symbol addresses per build with
`arm-none-eabi-nm -S`: they move when the layout changes. Used on 2026-09-19 to
show `500000 ok` for a 500-sample pulse, the same value the native build of
`src/scan.cpp` gives.
