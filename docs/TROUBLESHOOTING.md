# Troubleshooting

For wiring and normal use, see the [README](../README.md). For debug reports, see the [developer reference](DEVELOPER.md).

## Readings and sensor signal

Keep PA1 below 3.3 V + 0.3 V. The circuit runs from 3.3 V. Do not connect 5 V to PA1.

| Output | Cause | What to check |
|---|---|---|
| `0 clipped` | The dark, rail-side plateau is within 32 LSB of the ADC full-scale value. | Reduce the cascode gain or adjust its bias. Keep the dark plateau below 4063 LSB; about 100 LSB of headroom is a useful target. |
| `0 weak` | The plateau span is below 48 LSB, about 40 mV. | Add light and check alignment. Keep the lit plateau near `V_bias` (about 1.65 V). |
| `0 stale` | A crossing has no matching opening or closing crossing. | Check that the shutter makes one clear light pulse at the sensor. Reduce flicker and noise; point the sensor through the shutter and keep the setup steady. |
| Readings are too long, especially at fast speeds | The phototransistor may be saturated. Its collector-emitter saturation voltage is about 200 mV, and stored base charge delays turn-off. | Keep the phototransistor out of saturation. Check the bias and light level. Saturation recovery delays the closing edge. |

The SFH 309 FA is most sensitive near 900 nm (about 730–1120 nm).
Daylight, tungsten light, and xenon flash give useful infrared energy. White LEDs can give a weak signal.
The lens half-angle is about ±12°. Point it through the shutter.

The F103 ADC has 12-bit codes, but its effective resolution is closer to 9–10 bits. The firmware rejects a plateau span below 48 LSB rather than report a noise-driven result.

## No serial header or exposure lines

The firmware sends `shuttercheck exposure` at boot. USB CDC drops data sent before the host opens the port. Open the serial monitor, then press reset to see the header again.

After the header, the board prints one line for each capture. Check that the lens is pointed at the shutter and that light passes through it. A steady test light helps separate a sensor problem from flash variation. The onboard LED on PB12 flashes once for an accepted sample; it stays dark for a rejection.

If the board does not create a USB serial port, check the cable and board. The STM32F103 needs a 1.5 kΩ pull-up from PA12 to 3V3 for USB. The Black Pill has this resistor. The firmware USB CDC stack also needs the board to run, so press reset if a previous upload left the core halted.

## Upload and port access on Linux

### ST-Link access denied

If `pio run -t upload` reports `LIBUSB_ERROR_ACCESS`, add a udev rule for the ST-Link:

```sh
sudo tee /etc/udev/rules.d/60-stlink.rules >/dev/null <<'EOF'
# ST-Link/V2 programmers: give the users group write access.
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="3748", MODE:="0660", GROUP:="users"
EOF
sudo udevadm control --reload && sudo udevadm trigger
```

Unplug and reconnect the ST-Link. If the serial monitor cannot open `/dev/ttyACM0`, add your user to the `uucp` group and log in again:

```sh
sudo usermod -aG uucp "$USER"
```

### No ST-Link

The STM32 ROM bootloader uses a USB-serial adapter, not USB. Connect adapter 3V3 and GND, adapter TX to PA10, and adapter RX to PA9. Set `upload_protocol = serial` in `platformio.ini`. Set BOOT0 to 1, reset the board, and upload. Then set BOOT0 to 0 and reset again.

The F103 upload protocol list does not include `dfu`. USB uploads need a stm32duino (Maple) bootloader. That bootloader uses address `0x08002000` and identifies as `1EAF:0003`, not as the STM32 ROM DFU device.

### Board or toolchain differences

- A 128 KiB clone needs the `blackpill_f103c8_128` PlatformIO environment. The default environment targets a 64 KiB STM32F103C8T6.
- The Black Pill LED is PB12 and active-low. `LOW` turns it on. Blue Pill pin notes do not apply.
- OpenOCD 0.12 needs legacy HLA mode for ST-Link firmware older than V2J24. The project upload command uses `interface/stlink-hla.cfg`. After you update the ST-Link firmware, set `upload_protocol = stlink` in `platformio.ini` to use the stock flow.
- The project pins STM32duino core 3.0.0 with GCC 14.2. The core linker script needs the newer linker. Do not downgrade the toolchain by itself; GCC 9 can fail on `.ARM.extab` with a non-constant address expression.

See [TESTING.md](TESTING.md) for the on-device self-test and its ADC test wiring.
