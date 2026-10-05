# Testing

This guide covers the native scan tests and the on-device self test. The native tests need no board. The on-device test needs a Black Pill, an ST-Link, USB, and a test bridge for the ADC pulse checks.

## Native scan tests

Run the native test environment:

```sh
pio test -e native
```

PlatformIO generates the gitignored scan-vector header `test/test_data.h` before the build. The test compiles `src/scan.cpp` directly and runs the generated cases under both signal polarities. It checks scan results and rejection behavior without Arduino or STM32 hardware.

## On-device self test

The self-test checks the clock tree, capture start, USB CDC, generated scan cases, and the real ADC/DMA path. The scan cases run in both polarities. The ADC section generates pulses on PA0 and measures them through the capture path on PA1.

The script builds and flashes the test firmware, finds the USB CDC port, reads the report, and returns a status code. It needs PlatformIO, Python with `pyserial`, an ST-Link connected to SWD, and USB from the board to the host.

Before the ADC pulse section, disconnect the cascode output from PA1 and install this bridge:

| From | To | Purpose |
|---|---|---|
| PA0 | PA1 through 1 kΩ | Pulse source from TIM2_CH1. |
| PA1 | GND through 10 kΩ | Divider load. With the 1 kΩ series resistor, the high level is about 3.0 V (about 3723 ADC LSB). |
| PA1 | GND through 5 nF | Test capacitor. |

The high level (about 3723 LSB) stays below the 4063 LSB rail threshold. The 0-to-3723 LSB span is well above the 48 LSB `weak` floor. Do not replace the divider with a direct wire. A direct connection drives PA1 to the rail, so the scan correctly reports `clipped`. Without the bridge, the ADC pulse checks fail; the other checks can still run.

Run the complete test:

```sh
python scripts/on_device_test.py
```

The script generates the scan vectors, builds and flashes `on_device_test`, waits one second for USB to reconnect, then reads the report. It detects `/dev/ttyACM*` or `/dev/ttyUSB*` by default. Use `--port /dev/ttyACM0` to select a port, `--timeout 60` to wait longer, or `--no-flash` to read a board that already runs the test firmware.

The firmware waits about 10 seconds for a host to open the CDC port. The script opens the port after flashing. To read the report manually, generate the vectors, flash the environment, then open the monitor:

```sh
python scripts/gen_test_data.py
pio run -e on_device_test -t upload
pio device monitor
```

### Expected checks

The report checks these clock values:

| Clock | Expected |
|---|---:|
| SYSCLK and HCLK | 72 MHz |
| PCLK1 | 36 MHz |
| PCLK2 | 72 MHz |
| ADC clock | 12 MHz |
| USB clock | 48 MHz |

The pulse section first sends and discards a 5 ms conditioning pulse so the scan can learn both plateaus. It then measures 1, 2, 5, 10, 20, 50, 100, 250, 500, and 1000 ms pulses. Each width is a multiple of the 2 µs sample period, so the grid adds no width error. Each result must be within 0.5% of its nominal duration.

The final checks require zero overwritten DMA halves and a maximum scan time of 65,000 core cycles. A successful report ends with:

```text
overwritten halves 0
PASS DMA half backlog
scan max cycles 51974
PASS scan budget
ALL TESTS PASSED
```

A failed run ends with `FAILURES: <count>`. The script exits `0` on success, `1` when the board reports a failure, and `2` for a timeout or serial error. GitHub CI does not run this test because its workers have no board.

The `on_device_test` build checks the clock tree, capture path, USB stack, scan, and ADC/DMA path. It does not test the sensor front end or optical alignment.
