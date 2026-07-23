# Teensy AQ I2C sensor emulator

`teensy_aq_i2c_slave.ino` turns a Teensy 4.0 into an I2C **slave** that emulates an
air-quality sensor, so the driver's I2C path can be exercised on real hardware
without a production sensor. It answers at address `0x28` with the 18-byte
little-endian block the driver expects (layout documented at the top of the sketch),
and takes single-character serial commands to drive the demo.

## Wiring (Teensy 4.0 to Raspberry Pi)

Both run 3.3 V logic, so they connect directly. The Pi already has the I2C pull-ups.

| Teensy 4.0 | Raspberry Pi |
|---|---|
| pin 18 (SDA) | GPIO2 / header pin 3 (SDA1) |
| pin 19 (SCL) | GPIO3 / header pin 5 (SCL1) |
| GND | GND / header pin 6 |

**Do not** connect the Teensy's 3.3 V to the Pi. Power the Teensy from its own USB.

## Flashing

Easiest path is the Arduino IDE with Teensyduino installed: open the `.ino`, select
**Teensy 4.0** as the board, and upload.

From the command line with `arduino-cli`:

```bash
# one-time: add the Teensy board package and install the core
arduino-cli config add board_manager.additional_urls \
  https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core update-index
arduino-cli core install teensy:avr

# compile and upload (run from the driver/ directory; adjust the port for your machine)
arduino-cli compile --fqbn teensy:avr:teensy40 teensy_aq_i2c_slave
arduino-cli upload  --fqbn teensy:avr:teensy40 -p /dev/ttyACM0 teensy_aq_i2c_slave
```

The FQBN above is for a Teensy 4.0. Use the matching one for your board (for example
`teensy:avr:teensy41`). If the QNX sensor service is already polling the bus, stop it
first (`scripts/demo.sh stop`), otherwise the Teensy's reboot into the loader can be
starved by its I2C interrupt and the upload silently does nothing.

## Serial commands (115200 baud)

Send a single character over USB serial (or use `scripts/demo.sh teensy`):

- `p` pollute: ramp PM2.5 up to trip the cabin-filter alert
- `g` CO2: ramp CO2 up to trip the CO2-buildup alert
- `c` clear: return both back to baseline
- `s` state, `?` help
