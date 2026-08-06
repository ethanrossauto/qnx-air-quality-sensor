# Setup and build guide

The main [README](README.md) covers what the project is and how to run it. This guide
covers the parts that depend on the QNX toolchain: getting the SDP, placing the source
in the Sensor Framework example tree, building, reaching the target, and deploying.

## Prerequisites

- **QNX Software Development Platform 8.0**, installed on the host. You get it through the
  QNX Software Center with a myQNX account; QNX offers a **free non-commercial license** that
  covers this kind of personal and evaluation use. It provides `qcc`/`q++` (the aarch64 cross
  toolchain) and the target headers and libraries. The SDP runs on Linux and Windows.
- The helper scripts (`scripts/demo.sh` and friends) are `bash` and use standard Unix
  tools (`curl`, `nc`, `python3`). Run them from a Linux shell, or from WSL or Git Bash
  on Windows.
- **The QNX Sensor Framework and its examples package**, added through the QNX Software
  Center. The Software Center places it as `sf-sensor-examples-<version>.zip` (e.g. `0.3.0`)
  in `<SDP>/source/`; unzip it there to get the `source_package_sf_sensor/` tree that the
  driver and publisher build inside. The driver links `libsensor_utils`, the publisher
  links `libsensor` and `libsocket`.
- **A Raspberry Pi 4 or CM4 running QNX Everywhere (QNX 8.0)**, with `qconn` running
  (TCP port 8000). QNX Everywhere is the free evaluation image for the Pi.
- For the I2C demo, a **Teensy 4.0** flashed with the emulator (see
  [`driver/teensy_aq_i2c_slave/README.md`](driver/teensy_aq_i2c_slave/README.md)), or a
  real I2C air-quality sensor plus an implementation of `aq_bus_read_i2c` for its
  register map.

## Getting QNX onto the Pi

QNX Everywhere is the free evaluation image for the Raspberry Pi. From your myQNX account:

1. Download the QNX Everywhere Raspberry Pi image and flash it to a microSD card (8 GB or
   larger) with Raspberry Pi Imager or `dd`.
2. Boot the Pi from that card and set up networking so the host can reach it: join WiFi, or use
   the direct wired link described under "Reaching the target" below.
3. Confirm `qconn` is listening (it is what `demo.sh` talks to): from the host,
   `nc -z <pi-ip> 8000` should succeed.

Follow the QNX Everywhere quickstart for the current image download and board notes. The
official image targets the Raspberry Pi 4B; the CM4 works but needs a carrier/IO board.

## Placing the source in the SDP

This is purely additive: you drop two new directories into the example package and
reuse the sibling example's build files. No stock SDP file is modified. The parent
`Makefile` in each location is QNX's recursive build (`include recurse.mk`), which
descends into any subdirectory that has a `Makefile`, so a new directory is picked up
automatically with no parent edit.

Copy each of our directories next to the stock example it is modelled on, and reuse that
example's build files (`Makefile`, `common.mk`, `*.use`). Those build files are part of
the QNX package, so they are not shipped here; the driver's `Makefile` is byte-identical
to `example/Makefile`, and the publisher's start from `sensor_example/`'s with a few small
edits listed below.

**Driver** into the external-sensors tree, beside the stock `example/`:

```
<SDP>/source/source_package_sf_sensor/lib/sensor_drivers/external_sensors/aq/
```

- Copy `driver/aq_external_sensor.{h,cpp}` into that `aq/` directory.
- Copy the `Makefile` from the sibling `example/` directory into `aq/` unchanged.
- Nothing else: the recursive build finds `aq/` on its own.

**Publisher** into the sensor apps tree, beside `sensor_example/`:

```
<SDP>/source/source_package_sf_sensor/apps/sensor/aq_publisher/
```

- Copy `publisher/aq_publisher.cpp` into that `aq_publisher/` directory, along with a
  copy of `driver/aq_external_sensor.h` (the publisher includes it for the payload
  definition and flags).
- Copy the build files (`Makefile`, `common.mk`, `*.use`) from `sensor_example/`, then in
  the copied `common.mk`:
  - set `NAME=aq_publisher`,
  - add `EXTRA_INCVPATH += $(PROJECT_ROOT)` so it finds `aq_external_sensor.h`,
  - make the link line `LIBS += sensor socket` (the stock example links only `sensor`; the
    publisher also needs `socket` for the SSE server, or it will fail to link).
  - Rename the `.use` file to `aq_publisher.use`.

## Building

From each directory, with the SDP environment sourced:

```bash
source <SDP>/qnxsdp-env.sh
make CPULIST=aarch64
```

Outputs:

- Driver: `.../aq/nto/aarch64/so.le/libaq_external_sensor.so`
- Publisher: `.../aq_publisher/nto/aarch64/o.le/aq_publisher`

`scripts/demo.sh httpd` copies these two build outputs (plus the configs) into a staging
directory it serves over HTTP, so once they are built the deploy step just pulls them
onto the target.

## Reaching the target

`scripts/demo.sh` talks to the Pi over `qconn` (TCP 8000) and has the Pi pull the build
artifacts from an HTTP server on the host. Two addresses matter:

- `QNX_IP`: the Pi's address (where `qconn` listens).
- `LAPTOP_IP`: the host address the Pi should reach back to for the file pull. This must
  be the host's address **on the same link as the Pi**, not necessarily its main LAN IP.

Set both when the defaults in the script do not match your network:

```bash
QNX_IP=<pi-ip> LAPTOP_IP=<host-ip> ./scripts/demo.sh <subcommand>
```

To avoid repeating that on every command, copy `scripts/env.example.sh` to
`scripts/env.local.sh` and put your two addresses in it. Every script sources that file when it
is present, and it is gitignored, so your addresses stay out of the repo.

### The two helper scripts

`demo.sh` is the one you will use most, but it is built on two smaller helpers, and they are
useful on their own when something is not working:

| Script | What it does |
|---|---|
| `scripts/qsh.sh "<cmd>"` | runs a shell command on the target through `qconn` and prints its output. This is how you look around the Pi's filesystem, since SSH is not available. |
| `scripts/qrun.sh <abs-path> [args]` | launches a binary on the target by absolute path, setting `argv0` for you. |

Both read `QNX_IP` and `QNX_PORT` the same way `demo.sh` does.

Over WiFi this is just the two LAN addresses. For a **direct wired link** between the host
and the Pi (no DHCP server), give both ends a static link-local (`169.254.0.0/16`) address in
the same subnet:

```bash
# on the host: bring the wired NIC up and give it a link-local address
sudo ip link set <iface> up
sudo ip addr add 169.254.10.1/16 dev <iface>
```

QNX self-assigns a `169.254.x.y` address on an interface with no DHCP lease; read it from the
Pi's console with `ifconfig`, or set one explicitly there
(`ifconfig <pi-iface> 169.254.10.2 netmask 255.255.0.0`). Then run `demo.sh` with
`QNX_IP=169.254.10.2 LAPTOP_IP=169.254.10.1`. (There is no mDNS on QNX, so if you cannot read
the Pi's console, setting both addresses statically is the reliable path.)

## Running the host side on Windows (WSL)

The helper scripts are `bash` and expect Unix tools, so on Windows run everything from **WSL2**
(Ubuntu), not Git Bash (Git Bash has no `nc` and does not handle the serial/`stty` these scripts
use):

- Install the **Linux** SDP inside WSL and do the build and `demo.sh` from there.
- **Networking:** under default WSL2 NAT the Pi cannot reach back to the WSL VM for the file
  pull. Use Windows 11 **mirrored networking** (`.wslconfig`: `networkingMode=mirrored`), or add a
  `netsh interface portproxy` rule plus a Windows Firewall inbound allow on the HTTP port (8099).
  On a direct wired link the wired NIC and its `169.254` address live on the Windows host, while
  `LAPTOP_IP` must be the address WSL presents to the Pi.
- **Teensy:** flash and send serial commands from the Arduino IDE on Windows (the port is `COMx`,
  not `/dev/ttyACM0`); WSL only sees USB serial if you attach it with `usbipd-win`.

## Deploy and run

```bash
QNX_IP=<pi-ip> LAPTOP_IP=<host-ip> ./scripts/demo.sh httpd &   # serve the build outputs
QNX_IP=<pi-ip> LAPTOP_IP=<host-ip> ./scripts/demo.sh deploy    # target pulls them
QNX_IP=<pi-ip> LAPTOP_IP=<host-ip> ./scripts/demo.sh sim       # or: i2c
QNX_IP=<pi-ip> LAPTOP_IP=<host-ip> ./scripts/demo.sh publisher sim   # or: i2c
./scripts/demo.sh ivi                                          # http://localhost:8080
```

For the I2C bus, `demo.sh i2c` also muxes the Pi's 40-pin header I2C pins (GPIO2/GPIO3),
which the QNX Pi image does not mux by default, so `/dev/i2c1` is otherwise inactive.
Trigger the demo alerts from the Teensy with `scripts/demo.sh teensy` (`p`/`g`/`c`).
