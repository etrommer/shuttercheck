# AGENTS.md — shuttercheck firmware notes

Repo: camera shutter speed tester on a Blue Pill (STM32F103C8T6). User-facing
docs live in `README.md`; this file holds the build/flash commands, the
invariants the firmware must not break, and the facts already verified against
upstream sources so nobody has to re-derive them.

## Layout intent

- `src/main.cpp` — Arduino `setup()`/`loop()`. Wiring, range state machine,
  report formatting. No measurement arithmetic beyond unit conversion.
- The capture path is bare-register TIM3 code + `TIM3_IRQHandler`. Keep it in a
  separate translation unit from reporting so the ISR stays readable and its
  timing is obvious.
- Not yet created: `src/` does not exist, so `pio run` currently reports
  *nothing to build*. Do not add a placeholder sketch to silence that.

## Commands

```sh
pio run                      # build
pio run -t upload            # flash (default: ST-Link over SWD)
pio run -t clean
pio device monitor           # read reports on the USB CDC port
pio run -t upload -e bluepill_f103c8_128k   # 128 KiB clone
```

- PlatformIO is **not installed in this development environment** (`pio` is
  absent, `~/.platformio` does not exist). Build/upload verification must run
  where PlatformIO is installed; do not claim a build was verified from here.
- SWD wiring: 3V3, GND, PA13 (SWDIO), PA14 (SWCLK), NRST. `debug_tool`
  defaults to `stlink` from the board definition.
- Without an ST-Link: set `upload_protocol = dfu` in `platformio.ini`, jumper
  BOOT0 = 1, reset, upload via the F103 ROM bootloader, then BOOT0 = 0 + reset.
  The board definition already lists `dfu` among its upload protocols.
- 128 KiB clones: `board = bluepill_f103c8_128k`. Only the linker limit
  differs; `LED_BUILTIN` is PC13 for both (see below).

## Verified environment facts

Board/platform (from `platform-ststm32` board JSONs, `boards/bluepill_f103c8.json`
and `bluepill_f103c8_128k.json`): `mcu = stm32f103c8t6`, `cpu = cortex-m3`,
`f_cpu = 72000000L`, `extra_flags = -DSTM32F1 -DSTM32F103xB`, variant
`variant_PILL_F103Cx.h`, `maximum_size` 65536 (131072 for the `_128k` board),
RAM 20480, `upload.protocol = stlink` with
`protocols = jlink, cmsis-dap, stlink, blackmagic, mbed, dfu`.

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
TIM1 (APB2) and TIM2/3/4 (APB1, ×2) all count at **72 MHz**; `F_CPU` is
`72000000L`.

LED: the same variant defines `LED_GREEN = PC13` when `ARDUINO_BLUEPILL_F103C8`
or `ARDUINO_BLUEPILL_F103CB` is defined, and `LED_BUILTIN = LED_GREEN`. The
builder defines `ARDUINO_<BOARDID>` upper-cased (`platformio-build.py` L210-254),
so `bluepill_f103c8` → `ARDUINO_BLUEPILL_F103C8`. Do not rely on the board JSON
`extra_flags` for this. PC13 is active-low and is the RTC/TAMPER pin: drive the
on-board LED only, never a heavy load.

USB hardware: the F103 has **no internal D+ pull-up**; the board must fit
1.5 kΩ from PA12 to 3V3. Many Blue Pill clones populate `R10` with 10 kΩ (some
4.7 kΩ), which breaks or destabilises enumeration. Suspect this before
debugging USB CDC code.

## Design invariants

1. **Hardware timestamping only.** Both edges are latched by TIM3 in PWM-input
   slave mode on TI1 (PA6 = TIM3_CH1): IC1 latches on the rising edge into CCR1
   and, via `SMS = reset`, resets the counter; IC2 latches the falling edge into
   CCR2. The exposure is `CCR2` ticks. Never measure an edge by ISR entry time,
   never poll the pin, never attach an EXTI interrupt to PA6.
2. **Ranges:** `PSC ∈ {8, 719, 7199}` → 125 ns / 10 µs / 100 µs per tick, full
   scale 8.19 ms / 655 ms / 6.55 s. Start in the fastest range. On overflow,
   discard the sample, escalate one range and wait for the next pulse — never
   report a partial count. Step back down when a valid measurement is below half
   of the current range's full scale.
3. **Exactness of the overflow test:** the IC1 handler clears `UIF` and the
   stale capture flags, so `UIF` read at the falling edge means "the counter
   wrapped inside this pulse", and nothing else. Read the capture registers and
   the flags before clearing them, and clear by writing 0 to the specific bits.
4. **Edge bookkeeping:** a sample is valid only if exactly one rising and one
   falling edge were latched since the capture registers were last consumed.
   The falling edge of a pulse whose rising edge was never seen (power-up during
   an exposure) is `stale` and must be dropped. If **both** flags are already
   set when the rising-edge handler runs, the pulse completed before the ISR
   ran: report the latched `CCR2` and open a new pulse, so a short pulse is
   neither dropped nor misattributed.
5. **Conservative failure.** Every rejection path discards or escalates; the
   firmware never prints a value it has not verified. Losing a sample is
   acceptable, printing a wrong one is not.
6. **No interrupts-off stretches in the capture path.** ISRs are already
   interruptible. `SerialUSB.write()` blocks when the host is slow, so report
   from `loop()` only, never from an ISR, and keep a small result FIFO so a
   slow host cannot gate the capture. (Unlike the earlier ATtiny design, there
   is **no** serial-silent measurement window: hardware capture is immune to
   USB activity.)
7. **Integer math only** in the measurement path: carry ticks, convert with
   ns = ticks × {125, 10000, 100000}. No floats, no `millis()`/`micros()` in
   the measurement.
8. **Polarity:** dark → comparator output LOW, light → HIGH; rising = shutter
   opens, falling = shutter closes. Symptoms of a reversed input are a `stale`
   flood or a width that equals the dark interval — check the wiring, not the
   algorithm.
8. **Polarity:** dark → comparator output LOW, light → HIGH; rising = shutter
   opens, falling = shutter closes. Symptoms of a reversed input are a `stale`
   flood or a width that equals the dark interval — check the wiring, not the
   algorithm. A width that grows with the lamp level is front-end saturation
   (see below), also not an algorithm problem.
9. **Feedback:** one short flash on PC13 per accepted sample; nothing on
   rejection.

## Front end (analog)

Sensor is an **SFH 309 FA** Si NPN phototransistor: collector to 3V3, emitter
via `R_E` = 1 kΩ to GND, emitter node into LM393 +IN A, comparator output
(open-collector, 10 kΩ pull-up to 3V3) into PA6. The emitter follower is
non-inverting, so the firmware's polarity convention holds as long as the
sensor is wired collector-to-3V3.

- Datasheet numbers that drive the error budget: I_PCE 400…5000 µA over the
  bins at λ = 950 nm, E_e = 0.5 mW/cm², V_CE = 5 V; t_r/t_f 5…9 µs by bin at
  I_C = 1 mA, V_CC = 5 V, R_L = 1 kΩ; V_CEsat 200 mV; dark current 1 nA typ;
  spectral range 730…1120 nm, peak 900 nm; half angle ±12°.
- Keep the emitter plateau at 0.3…1 V. Saturation (node near 3V3) stores base
  charge and delays turn-off, which shows up as fast exposures reading *long*.
  That is a light-level problem, not something the firmware may compensate for
  or clamp — if readings drift with the lamp, say so instead of adding fudge.
- At 1/4000…1/8000 the dominant error is optical (slit width vs. chip size),
  not the timer. Do not chase it in code.

## Conflicting peripherals

`variant_PILL_F103Cx.h` reserves `TIMER_TONE = TIM3` and `TIMER_SERVO = TIM2`,
`SERIAL_UART_INSTANCE = 1` (PA9/PA10), SPI on PA5-PA7, I2C on PB6/PB7.

- Do not call `tone()` (would reconfigure TIM3), and do not `analogWrite()` on
  PA6 — its channel is TIM3_CH1 and PWM setup would rewrite TIM3's PSC/ARR.
- PA9/PA10 stay free: use them if a UART debug channel is ever wanted.
- PA11/PA12 are the USB pins; never reuse them for the front end.

## Verification policy

Firmware behaviour is verified **on hardware**, not by a host test suite:

```sh
pio run -t upload && pio device monitor
# fire the shutter, check the printed number against the dial setting
```

For a known reference without a camera, inject a 3.3 V pulse into PA6 (function
generator, or a second MCU) at 1/1000…1/8 and compare the report; check a
range escalation by injecting a pulse longer than 8.19 ms. If time-consumption
conversion or the range state machine grows host-testable logic (ticks → ns,
range decisions, sample rejection), add a PlatformIO `native` env and test it
there rather than on the board.

## Gotchas

- `USBD_USE_CDC` without the PIO flag: see above.
- `Serial.flush()` on this core waits for the host to drain the CDC buffer; it
  is not a no-op like on AVR.
- The ROM DFU bootloader needs the HSE crystal and PA11/PA12, and after a DFU
  upload the BOOT0 jumper must go back to 0 or the firmware never starts.
- `#include "Wire.h"`/`SPI.h` reserve PB6/PB7 and PA5-PA7; avoid enabling them.
