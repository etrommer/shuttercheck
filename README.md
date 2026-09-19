# shuttercheck

Camera shutter speed tester. An SFH 309 FA phototransistor senses the light
that goes through the shutter. A Black Pill (STM32F103C8T6) samples the
phototransistor output with its own 12-bit ADC on a timer-driven 2 µs grid.
The firmware rebuilds both shutter edges from the sample stream by
interpolation. There is no external comparator and no trimpot. The firmware
sends the measured exposure over USB CDC. Any serial terminal can read it.

[![PlatformIO CI](https://github.com/etrommer/shuttercheck/actions/workflows/ci.yml/badge.svg)](https://github.com/etrommer/shuttercheck/actions/workflows/ci.yml)

The capture path and the crossing scan operate at this time. The firmware
samples PA1 on the timer-driven 2 µs grid, rebuilds both shutter edges and
sends the measured exposure over USB CDC as `<exposure ns> ok` lines.
Rejections print `0 <status>`. `AGENTS.md` holds the build and flash commands
and the design invariants.

## How it measures

1. **SFH 309 FA** (Si NPN phototransistor, T1 3 mm radial). Connect the
   collector to 3V3 and the emitter to the load resistor `R_E` to GND. The
   current gain of the transistor multiplies the photocurrent. Thus the emitter
   node moves by hundreds of mV in normal test light. The node drives
   **PA1 = ADC1_IN1** through a 100 Ω series resistor. This resistor is the
   complete analog front end.
2. **Sampling grid.** A hardware timer starts one 12-bit ADC conversion each
   **2.000 µs** (500 kS/s). The sampling instants are hardware events. No
   software can move them.
3. **ADC and DMA.** The DMA writes each conversion result into a circular
   buffer when it arrives. Interrupt latency can never drop or retime the
   stream.
4. **Crossing detection.** The firmware scans each buffer chunk when it
   arrives. It finds the exact moment when the voltage crossed the threshold.
   It uses linear interpolation between the two samples that straddle the
   crossing:

   ```
   t_cross = (n − 1) × 2 µs + (V_thr − V_{n−1}) / (V_n − V_{n−1}) × 2 µs
   ```

   The firmware tracks the **dark plateau** (between
   pulses) and the **bright plateau** (during the pulse). It crosses at their
   midpoint. A software hysteresis band stops noise from faking an edge. The
   band is max(8 LSB, one sixteenth of the bright−dark span). A pulse re-arms
   only after the signal falls back to the low band edge. The firmware
   estimates the plateaus from samples that are not in a crossing pair and not
   in the band. The *crossing algorithm* section below walks through this
   detection step by step.
5. **Exposure.** The exposure is `t_cross(falling) − t_cross(rising)`. A
   sample is valid only if it has exactly one rising and one falling crossing.
   There are two rejection paths:
   - `clipped`: the bright plateau is within 32 LSB of full scale. The front
     end is saturated. The comparator design cannot see this fault.
   - `weak`: the bright−dark span is less than 48 LSB (≈40 mV). The midpoint
     would be noise.

There is **no range state machine**. One constant 500 kS/s grid covers all
speeds. At 1/4000 the pulse still gets ~125 samples for the interpolation. A
6 s Bulb is only a longer stream. Nothing can overflow.

| Speed     | Exposure | Samples/pulse | Raw grid error | With interpolation |
|-----------|----------|---------------|----------------|--------------------|
| 1/4000    | 250 µs   | 125           | ±0.8 %         | ≈0.05 % (noise-limited) |
| 1/1000    | 1 ms     | 500           | ±0.2 %         | ≈0.02 %            |
| 1/30      | 33 ms    | 16 700        | ±0.006 %       | threshold noise    |

Rated spec: **1/4000 … 6 s**. Faster speeds show correctly if the edges
resolve. But they are outside the accuracy budget below.

Polarity convention: dark gives a low voltage on PA1, light gives a high
voltage. The rising crossing is the shutter opening. The falling crossing is
the shutter closing. The phototransistor is an emitter follower. Thus the
front end is non-inverting.

### The crossing algorithm

The firmware processes one new sample at a time. Each step decides, in order:
the plateau estimates, the threshold and the hysteresis band, and the edges.
Most samples change no state except the sample index. This walkthrough
describes the full path of one sample.

The scan carries state across buffer chunks:

| Value              | Meaning                                          |
|--------------------|--------------------------------------------------|
| previous sample    | The last sample of the previous chunk            |
| dark plateau       | The EMA estimate of the dark level               |
| bright plateau     | The EMA estimate of the bright level             |
| locked threshold   | The threshold of the open pulse, fixed at its rise |
| opening time       | The interpolated time of the rising crossing     |
| rail latch         | A bright raw sample reached near full scale during the open pulse |
| absolute index     | The sample number over the whole capture         |

**Boot.** The first sample seeds the dark plateau. The shutter is closed at
power-on. Thus the first sample is the dark level. The bright plateau starts
16 LSB above it. The first real pulse then raises the bright estimate.

**The plateaus.** The firmware estimates a plateau with a moving average:

```
value = value + (sample − value) / 64
```

The division truncates. Thus the estimate moves in integer steps. Each
accepted sample moves the estimate one sixty-fourth of the way to the sample.
A noise tick of a few LSB moves the estimate by almost nothing. The previous
sample enters the dark estimate only when it lies below the low band edge. It
enters the bright estimate only when it lies above the high band edge.
Samples in the band and samples in a crossing pair are excluded: they belong
to neither plateau.

**The band.** The threshold is the midpoint of the two estimates:

```
thr = (dark + bright) / 2
```

The hysteresis band is `max(8 LSB, (bright − dark) / 16)`. The band edges
sit at `thr ± band/2`. The band has two jobs. It stops noise from faking a
rise: a rising edge needs a step of at least half the band. It keeps the
plateaus clean: only samples clearly on one side of the band feed an
estimate.

**The edges.** A crossing needs the previous sample on one side and the new
sample on the other side of the threshold.

- **Rising (shutter opens).** The previous sample must lie at or below the
  low band edge, and the new sample must reach the threshold. The low-edge
  condition is the re-arm: a rise can fire only after the signal came back
  below the band. At the rise, the firmware locks the threshold for this
  pulse and records the interpolated opening time.
- **Falling (shutter closes).** While a pulse is open, the new sample must
  fall below the locked threshold. The firmware interpolates the closing
  time and computes the exposure:

  ```
  exposure = closing time − opening time
  ```

- **Stale.** A falling pattern with no open pulse is a lone falling
  crossing. The firmware reports `stale` and drops it.

Both crossing times come from the interpolation formula in step 4.

**Why the threshold is locked.** The midpoint can move while a pulse is open.
A light level that drifts (for example xenon flash decay) biases a moving
threshold. The locked threshold crosses both edges at the same fraction of
the swing. Measurement between equal-fraction crossings cancels the shape of
the optical edge. See the plateau estimation row in Accuracy above.

**Validity and rejections.** A sample is valid only when exactly one rising
and one falling crossing close one pulse. Two rejection paths remain.

- `clipped`: while a pulse is open, any raw sample at or above 4063 (full
  scale minus 32) latches the rail flag. The firmware checks the raw samples,
  not the bright estimate. The integer estimate stalls about 64 LSB below a
  constant level. Thus it can never enter the 32 LSB rail zone by itself.
- `weak`: the bright−dark span is below 48 LSB.

Rejected pulses print `0 clipped` or `0 weak`.

**One optimization.** Runs of consecutive samples that can change no state
are skipped as one block. A run is skippable when no pulse is open and every
sample of the run either stays strictly inside the band or equals its
plateau. Such samples cannot start an edge and cannot move an estimate. The
walkthrough above describes the full path. Most samples take the short path.

### Output

One line for each capture on the USB CDC port. The line holds nanoseconds and
then a status.

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

The LED on PB12 gives one short flash for each accepted sample. It stays dark
for a rejection. `2481234 ns` = 2.481 ms ≈ 1/403 s. Send the output to any
serial terminal (`pio device monitor`). No host-side software is necessary.

## Required hardware

| Part | Notes |
|------|-------|
| Black Pill board with [STM32F103C8T6](https://www.st.com/en/microcontrollers-microprocessors/stm32f103c8.html) | 64 KiB flash. For a 128 KiB clone, build the `blackpill_f103c8_128` env |
| [SFH 309 FA](https://ams-osram.com/products/photodetectors/phototransistors/osram-radial-t1-sfh-309-fa) phototransistor ([datasheet](https://look.ams-osram.com/m/48f3c58ae57c5dfc/original/SFH-309.pdf)) | Si NPN, T1 3 mm radial. Point the lens at the shutter |
| [ST-Link/V2](https://www.st.com/en/development-tools/st-link-v2.html) programmer | Flashes the board over SWD. Any STM32 SWD probe works |
| `R_E` 1 kΩ, series 100 Ω, 1 nF (optional) | The series resistor is mandatory. The capacitor filters HF noise only |
| USB cable, board 3V3/GND | The board supplies the front end. Keep 5 V away from PA1 |

## Wiring

The complete front end operates from **3V3**. Keep 5 V away from the MCU pin.
PA1 is an ADC pin. Never let the node go above 3V3 + 0.3 V.

| From                        | To                                | Notes |
|-----------------------------|-----------------------------------|-------|
| SFH 309 FA collector        | 3V3                               | Long lead |
| SFH 309 FA emitter          | node A                            | Pin 1 / short lead. Point the lens at the shutter. The half angle is only ±12° |
| `R_E` 1 kΩ                  | node A → GND                      | Controls amplitude and speed. 1 kΩ is the t_r/t_f condition in the datasheet. 100 Ω is faster, but it needs more light |
| Series 100 Ω                | node A → PA1                      | Mandatory. Limits the ADC sampling-charge kick and the fault current. The ADC sample time is dimensioned for the total ≈1.1 kΩ source impedance |
| 1 nF (optional)             | PA1 → GND, at the pin             | HF noise only. Do not use 100 nF here: with `R_E` = 1 kΩ, that value gives a 100 µs time constant and it smears a 250 µs pulse. 1 nF adds ≈1 µs of delay to both edges. The delay cancels in the width |
| Black Pill                  | USB → PC                          | The board 3V3/GND supply the front end |

### Hardware build

Build the sensor head first. Then wire it to the board. Then flash and check.

1. Build on breadboard or perfboard. Keep all leads short.
2. Connect the phototransistor collector (long lead) to 3V3. Connect the emitter (short lead) to node A.
3. Connect `R_E` 1 kΩ from node A to GND.
4. Connect node A to PA1 through the 100 Ω series resistor. Do not omit it. It limits the sampling kick and the fault current.
5. Optional: fit 1 nF from PA1 to GND at the pin. It filters HF noise only. Do not use 100 nF: it smears a 250 µs pulse.
6. Power the front end from the board 3V3 and GND. Keep 5 V away from PA1. Never exceed 3V3 + 0.3 V on the pin.
7. Wire the ST-Link to SWD (3V3, GND, PA13 = SWDIO, PA14 = SWCLK) and flash with `pio run -t upload`.
8. Connect the board USB to the PC and open `pio device monitor`. Expect the `shuttercheck exposure` header line.
9. Point the lens at the shutter. The half angle is only ±12°. Shine a steady test light through the shutter at the sensor.
10. Set the light level so the bright plateau stays between 0.3 V and 1 V. If the monitor prints `clipped`, dim the source. If it prints `weak`, add light.
11. Fire the shutter. Expect one `<exposure ns> ok` line and one LED flash per accepted sample.

Front-end sizing:

- **Keep the emitter plateau between 0.3 V and 1 V.** At 3 V the
  phototransistor is saturated. `V_CEsat` is 200 mV and the stored base charge
  delays the turn-off. Thus the closing edge reads late and fast exposures look
  long. The ADC makes this failure visible, unlike the comparator design. The
  firmware sees samples near full scale and prints `clipped`. Dim the source
  (lens aperture, distance, diffuser) until the plateau reads well below full
  scale.
- **Noise floor.** One LSB is 0.8 mV. The effective resolution of the F103 is
  closer to 9–10 bits. Thus the firmware rejects a plateau span below ~40 mV as
  `weak`. It does not report a meaningless midpoint.
- **Light source spectra.** The sensitivity peaks at 900 nm
  (range 730…1120 nm). Daylight, tungsten and xenon flash are ideal. White LEDs
  are weak at that wavelength.

## Accuracy

| Term                      | Contribution |
|---------------------------|--------------|
| 8 MHz HSE crystal         | ±30 ppm → 0.003 %. Negligible |
| Sample grid               | ±2 µs worst case between raw samples: 0.8 % at 1/4000 and 0.2 % at 1/1000. This value is the upper bound without interpolation. It is not the operating error |
| Interpolation residual    | Crossing jitter ≈ 1 LSB of voltage noise ÷ edge slope. With optical edges of ~20 mV/µs, that is tens of ns for each edge. The grid has no jitter. Conversions start on timer events |
| Phototransistor t_r/t_f   | 5…9 µs, dependent on the bin (datasheet: I_C = 1 mA, V_CC = 5 V, R_L = 1 kΩ). Both edges have about the same delay. Thus the delay largely cancels in the width. The residual comes from rise/fall asymmetry and saturation recovery |
| Optical edge shape        | **Dominant at fast speeds.** At 1/4000 the slit is only a little wider than the chip. Thus the light at the sensor is a trapezoid with edges tens of µs wide. Measurement between equal-fraction crossings cancels this shape. The auto-midpoint threshold is an equal-fraction crossing. The firmware computes it again for each pulse, instead of a trimpot set by eye |
| Plateau estimation        | The midpoint is only as good as its two plateaus. A light level that changes during a pulse or between pulses (notably xenon flash decay) biases `V_thr` and the width. The result is reproducible only with a steady source |

Bottom line: ~0.1 % class from 1/30 to 1/2 with a repeatable setup. Expect a
few percent at 1/4000. At that speed the geometry of the shutter slit dominates
and the sampling chain adds a few × 0.1 %. Remember the sign of the error: a
saturated front end biases each fast reading long. But the firmware also prints
`clipped`. That status is the reason to sample the voltage and not only to
compare it.

### What it does not measure

- **One point in the frame.** A focal-plane shutter exposes different points at
  different times. The sensor sees the light at its own position. Thus you get
  the local exposure time, not the curtain-travel uniformity across the frame.
- Flash sync timing. Also nothing about the lens aperture.
- Absolute exposure. A midpoint between two plateau estimates is not a
  photometric standard. Use the tester for comparison: same setup, compare
  against nominal or against another camera body.

## Build, flash, run

You need [PlatformIO](https://platformio.org/) (`pipx install platformio`, or the VS Code extension).

1. Clone the repo and enter it:

```sh
git clone https://github.com/etrommer/shuttercheck.git
cd shuttercheck
```

2. Build the firmware. Run the unit tests with `pio test -e native`.

```sh
pio run                 # build the default env, blackpill_f103c8
pio run -e blackpill_f103c8_128   # build a 128 KiB clone
pio test -e native     # run the native scan unit tests
pio run -t upload       # flash over ST-Link (SWD: 3V3, GND, PA13 = SWDIO, PA14 = SWCLK)
pio device monitor      # read the reports from /dev/ttyACM0
```

3. Flash the board over ST-Link, then open the monitor. Expect this header line, then one line per capture:

```
shuttercheck exposure
2481234 ok
```

The upload ends with an OpenOCD reset. Thus the board runs the new firmware at
once. Without that reset the core stays halted after the flash: the board still
enumerates on USB, but it runs nothing until you press reset.

The header goes out at boot. The CDC port drops everything it sends before a
host opens the port. Thus you miss the header when you open the monitor later.
Press reset to see it again.

### On-device self test

The `on_device_test` environment builds a firmware that tests itself. It
starts the capture path, but it feeds the generated scan vectors to the scan
instead of the ADC output. Thus it needs no sensor and no light. It checks the
clock frequencies, the capture path, the USB connection and the eight scan
cases, and it prints one line per check:

```sh
python scripts/gen_test_data.py      # generate the scan vectors
pio run -e on_device_test -t upload  # build and flash
pio device monitor                   # read the report
```

```
PASS sysclk 72 MHz
PASS hclk 72 MHz
PASS pclk1 36 MHz
PASS pclk2 72 MHz
PASS adcclk 12 MHz
PASS usbclk 48 MHz
PASS capture running
PASS usb host connected
PASS scan case 0
...
PASS scan case 7
ALL TESTS PASSED
```

The LED blinks forever when a check fails. It stays dark when everything
passes. The report waits up to 10 s for a host to open the CDC port. Open the
port, or press reset, inside that window.

One command does all of it. It builds, flashes, reads the report and exits
non-zero on a failure or on a timeout. It needs the board on SWD and on USB,
and Python [pyserial](https://pypi.org/project/pyserial/):

```sh
python scripts/on_device_test.py
```

GitHub CI does not run it: the runner has no board.

### USB permissions for the upload

On Linux, `pio run -t upload` fails with `LIBUSB_ERROR_ACCESS` if your user
cannot open the ST-Link device. Install a udev rule:

```sh
sudo tee /etc/udev/rules.d/60-stlink.rules >/dev/null <<'EOF'
# ST-Link/V2 programmers: give the users group write access.
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3748", MODE:="0660", GROUP:="users"
EOF
sudo udevadm control --reload && sudo udevadm trigger
```

Then unplug the ST-Link and plug it in again. The USB CDC serial port
(`0483:5740`) usually needs no rule. If `pio device monitor` cannot open
`/dev/ttyACM0`, add your user to the `uucp` group and log in again:
`sudo usermod -aG uucp $USER`.

No ST-Link and no stm32duino bootloader? The ROM bootloader of the F103 needs
no extra hardware, but it does not operate over USB. Do these steps:

1. Wire a USB-serial adapter (3V3, GND, TX→PA10, RX→PA9).
2. Set `upload_protocol = serial` in `platformio.ini`.
3. Put the BOOT0 jumper at 1.
4. Reset the board.
5. Upload the firmware.
6. Return BOOT0 to 0 and reset the board again.

`dfu` is not in the upload protocol list of this board. A USB upload first
needs the stm32duino (Maple) bootloader at 0x08002000. Even then the board
enumerates as `1EAF:0003`, not as the ST ROM DFU device. Full command matrix
and design invariants: [`AGENTS.md`](AGENTS.md).

### Board notes to know before you blame the firmware

The STM32F103 has no internal D+ pull-up. Thus the board must have **1.5 kΩ
from PA12 to 3V3**. The Black Pill has this resistor. Unlike the Blue Pill,
there is no "measure `R10` before debugging" trap here. A dead CDC port points
to the firmware or the cabling. The regulator is an ME6211 (3.3 V, 180 mA).
That value is sufficient because the front end takes ~1 mA. Use
`blackpill_f103c8_128` in the board list if your board has the 128 KiB part.
The user LED is **PB12** on the Black Pill (PC13 on the Blue Pill). It is
active-low and sinks through the pin. Thus `LOW` lights it.

Two build facts that cost time when unknown:

- **ST-Link firmware older than V2J24.** OpenOCD 0.12 drives those dongles
  only in legacy HLA mode. A V2J17S4 dongle works with
  `interface/stlink-hla.cfg`, which is what `platformio.ini` uses. Plain
  `interface/stlink.cfg` selects the newer dapdirect driver and it refuses the
  dongle. After updating the dongle firmware (STSW-LINK007), set
  `upload_protocol = stlink` to go back to the stock flow.
- **gcc 14.2 goes with STM32duino core 3.0.0.** The core linker script needs
  the newer `ld`. With gcc 9 the link fails on `.ARM.extab` (non constant
  address expression). Do not downgrade the toolchain alone.
