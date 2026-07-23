# Air Quality in the QNX Sensor Framework

A working example that brings an **air-quality sensor into the QNX Sensor Framework**, served
alongside its built-in sensor types, running on QNX Everywhere on a Raspberry Pi, and surfaced on a
car-cabin "infotainment" screen that turns from green to red when the cabin air is no longer clean.

![Cabin-air IVI, clean cabin](docs/ivi-ok.png)
![Cabin-air IVI, filter alert reading a real I2C sensor](docs/ivi-alert.png)

*The infotainment screen: green while the cabin air is clean, and red with a "cabin air filter no
longer protecting occupants" alert when particulates climb. The second screen is reading a real
sensor over I2C, shown by `LIVE · I2C` in the top bar.*

The Sensor Framework ships with native formats for cameras, LiDAR, radar, GPS, IMU, and CAN, but
there is no built-in air-quality format. This project rides on `SENSOR_FORMAT_USER_DATA` with a
fixed, packed `aq_sample_t` payload, which is the framework's supported path for a sensor type it
does not model natively. The result is a real air-quality sensor plugged into the same service,
config, and client APIs that the built-in sensors use.

## The pipeline

```
  air-quality sensor              QNX Sensor Framework                 HMI
  (I2C, or a software    ->   external_sensor driver (.so)   ->   libsensor client
   simulator)                 emits SENSOR_FORMAT_USER_DATA        (publisher) decodes
                              via the sensor service               the payload and serves
                                                                   an HTTP SSE stream
                                                                            |
                                                                            v
                                                                   browser cabin-air IVI
                                                                   (green / amber / red +
                                                                    filter alert banner)
```

Every stage is real QNX plumbing. The only hardware-specific code is a single `aq_bus_read()`
function, so moving from the simulator to a real part means implementing one bus read and nothing else.

## What's in here

| Path | What it is |
|------|-----------|
| `driver/` | The QNX `external_sensor` air-quality driver (`aq_external_sensor.{h,cpp}`). Bus-agnostic, with a live software simulator and a real I2C read path. |
| `driver/teensy_aq_i2c_slave/` | A Teensy I2C-slave sketch that emulates an air-quality sensor, so the real I2C path can be exercised on actual hardware using a stand-in sensor. |
| `publisher/` | `aq_publisher.cpp`, a `libsensor` client that subscribes to the sensor unit, decodes the payload, and re-publishes each sample as an HTTP Server-Sent-Events stream. |
| `ivi/` | A single-file browser car-infotainment screen that consumes the SSE stream and shows the cabin-air state and alerts. No build step. |
| `configs/` | Sensor-service config blocks for the simulator and I2C buses. |
| `scripts/` | `demo.sh` to deploy and drive the whole demo over qconn, plus small qconn helpers. |

## Two alerts

1. **Cabin filter / particulate**, driven by PM2.5. When cabin particulates climb past the
   threshold the screen goes red with a "cabin air filter no longer protecting occupants" banner.
   This sits on a real particulate reading.
2. **Cabin CO2 buildup**, driven by CO2, for the case where occupants breathe up a sealed cabin on
   recirculation. This is a roadmap-level feature: a sensor that reports estimated eCO2 derived from
   a VOC channel is not a substitute for true NDIR CO2, so a trustworthy version needs a real NDIR
   CO2 sensor. It is demonstrated here with simulated data and clearly marked as such in the code.

## Requirements

### Hardware

- A **Raspberry Pi 4** (recommended; the official QNX image targets the 4B) or a **CM4** with a
  carrier board, plus its power supply and a **microSD card (8 GB or larger) and a way to flash it**.
- A **host computer** (Linux, or Windows via WSL) with a network link to the Pi: WiFi, or
  an Ethernet cable for a direct wired link.
- For the real I2C path, one of:
  - a **Teensy 4.0** flashed as the sensor emulator (a bare Teensy ships without headers, so you
    need soldered headers or test clips), or
  - a real **I2C air-quality sensor**.
- **3 female-to-female jumper wires** for the I2C link (SDA, SCL, GND), plus a **USB cable** for the
  Teensy (micro-USB on the Teensy 4.0).

The **simulator bus needs no sensor hardware at all**, just the Pi, so you can run the
whole pipeline before wiring anything up.

### Software

- **QNX Everywhere (QNX 8.0)** on the Pi (the free evaluation image).
- The **QNX Software Development Platform 8.0** with the Sensor Framework examples package
  on the host, to build the driver and publisher. Full steps in [`SETUP.md`](SETUP.md).
- To flash the Teensy, the Arduino toolchain (see
  [`driver/teensy_aq_i2c_slave/README.md`](driver/teensy_aq_i2c_slave/README.md)).

## Try it

The whole pipeline runs with no sensor hardware at all, on the software simulator bus:

```bash
# from your host; these drive the QNX target over qconn
./scripts/demo.sh httpd &      # serve the build artifacts to the target
./scripts/demo.sh deploy       # copy the driver, configs, and publisher onto the target
./scripts/demo.sh sim          # start the sensor service on the simulator bus
./scripts/demo.sh publisher sim

# serve the IVI and open it in a browser
./scripts/demo.sh ivi          # http://localhost:8080
```

Force the filter alert with `./scripts/demo.sh sim-pollute` then `./scripts/demo.sh publisher sim`
(`sim-pollute` restarts the sensor service, so restart the publisher too); `./scripts/demo.sh sim-co2`
does the same for the CO2 alert. The browser IVI also has a self-contained **Demo** mode with pollute
and CO2 buttons, so you can see both alerts with nothing else running.

To exercise the real I2C path, flash the Teensy emulator and wire it to the Pi's I2C pins (wiring and
flashing are in [`driver/teensy_aq_i2c_slave/README.md`](driver/teensy_aq_i2c_slave/README.md)), then
use the `i2c` subcommands instead of `sim`.

Set `QNX_IP` and `LAPTOP_IP` to match your network before running `demo.sh` (the addresses in the
script are examples).

## Building the driver

The driver builds in the Sensor Framework SDP package tree with `qcc` (aarch64le) into
`libaq_external_sensor.so`, and is loaded by referencing the `.so` from a `SENSOR_UNIT` block.
**[`SETUP.md`](SETUP.md) is the full setup guide**: SDP prerequisites, where the source goes in the
example tree, building, reaching the target, and deploying. Per-component notes are in
[`driver/README.md`](driver/README.md) and [`publisher/README.md`](publisher/README.md).

## Status

The driver compiles clean with `qcc` and streams `SENSOR_FORMAT_USER_DATA` packets on a real QNX
target (Raspberry Pi CM4), both on the software simulator bus and over a real I2C bus driven by the
Teensy emulator. The alert path fires end to end through the driver, the publisher, and the browser
IVI. Sensor data in this repository is simulated or emulated; wiring in a production sensor is a
matter of filling in the one bus-read function for that part.

## Acknowledgments

Developed in collaboration with [EcoSafeSense](https://ecosafesense.com).

## License

MIT. See [`LICENSE`](LICENSE).
