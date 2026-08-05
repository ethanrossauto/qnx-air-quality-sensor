# Air quality as COVESA VSS signals on the QNX signal service

An air-quality sensor on a physical I²C bus, published as **COVESA-VSS-named signals** in
the QNX signal service, readable with `cat`.

```
$ cat /dev/qpp/Vehicle/Cabin/AirQuality/PM25?text
119.000000
```

That path is not invented for this project. It comes from a **COVESA proposal filed from this
repository and since merged into the standard**, which until then had no air-quality signals at all:

- [Issue #930](https://github.com/COVESA/vehicle_signal_specification/issues/930), describing the gap
- [PR #931](https://github.com/COVESA/vehicle_signal_specification/pull/931), the change itself

To be exact about status: **merged is not the same as released.** The proposal was reviewed,
approved and merged into COVESA `master` on 2026-08-05, validated against COVESA's own
toolchain. It is not yet in a tagged release, so a consumer pinned to v6.0 or v5.1 will not
see these signals. The signals
work today only because the QNX signal service is happy to boot a custom catalog.

## The problem this solves

The QNX Sensor Framework is designed for high-bandwidth streaming devices: cameras, LiDAR,
radar. An air-quality sensor produces a handful of scalars at about 1 Hz. It fits, but
awkwardly. The `../driver/` tree in this repository does exactly that, riding on
`SENSOR_FORMAT_USER_DATA`, and it works. The catch is the data model: every consumer needs
a copy of a private packed struct to make sense of the bytes.

The **signal service** is the right home for data of this shape. It is COVESA-VSS-based, so
a consumer looks up `Vehicle.Cabin.AirQuality.PM25`, gets a float in µg/m³, and needs no
private header. Signals are files; the whole client API is POSIX.

So there are two questions, and this folder answers both:

1. **Which VSS signal does an air-quality sensor map to?** None. See below.
2. **How do you get the sensor into the signal service?** Two working programs, in
   `connector/`.

## Finding 1: COVESA VSS had no air-quality signals, and no concentration units

This was the gap the proposal closed. Checked across VSS `master`, `v6.0` and `v5.1` **as they
stood when this was written**. `master` has since merged the signals below:

- **Zero air-quality signals.** No PM, CO2, VOC, NO2, ozone, or AQI anywhere in the cabin
  or ambient branches. The only CO2 in the catalog is `Vehicle.EmissionsCO2`, a tailpipe
  g/km attribute, plus diesel particulate *filter* temperatures.
- **Zero concentration units.** No ppm, ppb, or µg/m³ in `units.yaml`, and no mass-density
  quantity in `quantities.yaml`. So this is not just a missing leaf; the shared unit
  infrastructure is missing too.
- **No open proposals.** No issues or PRs proposing any of it.

Method and evidence: [`docs/qnx-signal-service.md`](docs/qnx-signal-service.md).

## Finding 2: you do not have to wait for COVESA

QNX's own reference catalog carries custom signals outside the COVESA tree
(`Vehicle/QNXCustom/*`). The signal service does not care whether a catalog is standard,
only that it parses. So the overlay in `proposal/overlay/` is usable today, and the
proposal is about making the signals *standard* rather than about making them *work*.

## What's here

| Path | What it is |
|---|---|
| `proposal/` | The COVESA proposal: 14 signals (7 measurements × Cabin and Exterior), the three missing units, and a `mass-per-volume` quantity. This is what was filed as [PR #931](https://github.com/COVESA/vehicle_signal_specification/pull/931), and it also works as a standalone overlay against an unmodified catalog. Design rationale in [`proposal/PROPOSAL.md`](proposal/PROPOSAL.md). |
| `connector/aq_signal_publisher.c` | **The direct path.** Opens the I²C bus, reads the sensor, writes the signal files. No Sensor Framework anywhere in the process. |
| `connector/aq_signal_connector.c` | **The bridged path.** Subscribes to an existing QNX Sensor Framework unit and republishes it as VSS signals. Use this if the sensor is already a framework unit. |
| `connector/Makefile` | Builds both, relative paths only. |
| `catalog/` | The 17-signal QPP catalog the demo boots from, plus the script that regenerates it from any vss-tools JSON export. |
| [`VALIDATION.md`](VALIDATION.md) | What was run against COVESA's own toolchain, with versions, and what was **not** validated. |
| [`VERIFIED_ON_TARGET.md`](VERIFIED_ON_TARGET.md) | What ran on real hardware, the measured values, and what is still a stand-in. |
| [`docs/qnx-signal-service.md`](docs/qnx-signal-service.md) | What the QNX signal service actually is (QPP), its real API, and how it differs from the Sensor Framework. |

## Which connector do I want?

**Start from a bare I²C device →** `aq_signal_publisher`. One process, one bus, no
framework. This is the shape that matches what the signal service is for.

**Already have a Sensor Framework unit →** `aq_signal_connector`. It bridges what you have
rather than making you rewrite it.

Both write the same signals, so consumers cannot tell them apart, which is the point.

## Running it

### What you need first

- **QPP built for your target.** It is not in the QNX SDP. Clone
  [github.com/qnx/qnx-posix-publish-subscribe](https://github.com/qnx/qnx-posix-publish-subscribe),
  source your SDP environment, and run `make`. That produces a `qpp` binary per
  architecture; you want the one matching your target (`aarch64le` for a Pi).
- **The connectors built**, below.
- A way to reach the target. This repository drives QNX over `qconn` using
  `../scripts/qsh.sh`, and moves files by serving them over HTTP from the host and pulling
  them with `curl` on the target. QNX Everywhere images have no ssh, which is why it works
  this way.

### Build

```bash
source <your-sdp>/qnxsdp-env.sh
cd connector

make publisher    # the direct path. Needs only the SDP.
make              # both. The bridge links libsensor, so this additionally needs
                  # the Sensor Framework example package installed (see ../SETUP.md).
```

### Deploy and run

`../scripts/demo.sh` has subcommands for this path. From the repository root, with
`QNX_IP` and `LAPTOP_IP` set for your network and `QPP_BIN` pointing at the `qpp` binary
you built:

```bash
export QPP_BIN=/path/to/your/qpp-aarch64le

./scripts/demo.sh vss-stage      # collect qpp, the catalog and the connectors
./scripts/demo.sh httpd &        # serve them to the target
./scripts/demo.sh vss-deploy     # pull them onto the target

./scripts/demo.sh mux            # mux the I2C header pins (Raspberry Pi; see below)
./scripts/demo.sh qpp            # start the signal service with the air-quality catalog
./scripts/demo.sh vss-publish    # direct path: I2C -> signal service

./scripts/demo.sh vss-read       # cat a signal back
./scripts/demo.sh vss-read Vehicle/Cabin/AirQuality/CO2
./scripts/demo.sh vss-stop       # stop the publisher and the signal service
```

For the bridged path instead, start the sensor service first
(`./scripts/demo.sh i2c`), then `./scripts/demo.sh qpp`, then
`./scripts/demo.sh vss-connect`. Order matters: the bridge binds to whichever sensor-service
instance is running when it starts, and does not re-subscribe if that service is restarted.

### Or by hand, if you are not using the scripts

```bash
# on the target
qpp -s -c qpp_catalog_airquality_demo.json -m /dev/qpp
aq_signal_publisher -b <i2c bus> -a <7-bit addr> -m /dev/qpp -f 1 -v

# read a signal, from anywhere, with anything.
# quote the path so the shell does not try to glob the '?'
cat '/dev/qpp/Vehicle/Cabin/AirQuality/PM25?text'
od -t f4 -N 4 /dev/qpp/Vehicle/Cabin/AirQuality/PM25
```

The bus and address for the demo rig are in [`../configs/aq_i2c.conf`](../configs/aq_i2c.conf).

**I²C pin muxing.** The QNX Raspberry Pi image does not mux the 40-pin header's I²C pins, so
`/dev/i2c1` is dead until you do it. `./scripts/demo.sh mux` runs exactly these two commands
on the target, and nothing else:

```
gpio-bcm2711 set 2 a0 pu
gpio-bcm2711 set 3 a0 pu
```

## Using your own sensor

**The wire protocol lives in one function.** In `aq_signal_publisher.c` that is
`aq_i2c_read()`: it does a single block read and decodes fixed little-endian offsets.
Everything downstream works off the decoded `aq_reading_t`, so a different protocol changes
nothing else. Two things that trip people up, both called out in the source:

- **Many real parts need more than a bare read.** A write-then-read to select a register
  (`DCMD_I2C_SENDRECV`), a start-measurement command when the bus is opened, and per-word
  CRC-8 instead of one trailing sum byte are all common. That work sits inside
  `aq_i2c_read()` and `aq_i2c_open()`; nothing above them changes.
- **Set `valid_mask` to the channels your part actually measures.** Signals whose bit is
  clear are never written, so a PM-only sensor leaves CO2 and NO2 untouched instead of
  publishing a fake `0.0`. "No reading" and "a reading of zero" are different claims, and
  a consumer cannot tell them apart after the fact.

**To publish a different set of signals**, add a row to `g_signals[]`. Each row carries the
VSS path, the valid bit, and where the value lives in `aq_reading_t`, so one row really is
all it takes.

The signal must also exist in the catalog that `qpp` booted from. Two ways:

- **Hand-edit `catalog/qpp_catalog_airquality_demo.json`.** It is a plain vss-tools export,
  readable, and this is the quick path.
- **Regenerate it** with `catalog/make_qpp_catalog.py` after editing its `KEEP` list. This
  is the reproducible path, but it needs a VSS checkout plus vss-tools installed from git
  master and the overlay applied. The exact command is Run 3 in
  [`VALIDATION.md`](VALIDATION.md), and note that the released vss-tools 6.0.0 from PyPI
  will not work against current VSS master.

If your part reports true TVOC in ppb rather than a dimensionless VOC index, build with
`make VOC_IS_PPB=1` so the TVOC signal is published. It is deliberately left unpublished by
default, because writing an index into a signal declared in ppb would be wrong.

## Honest scope

Some things are deliberately not published, and the reasoning is in the source:

- **Cabin relative humidity** is read off the wire but has no home in VSS. The standard has
  `Exterior.Humidity` only. Publishing it somewhere approximate would be worse than not
  publishing it.
- **AQI and alert status** are derived policy, not measurements. Alert logic belongs in a
  policy component, not a data publisher.
- **CO2 from low-cost parts is often estimated eCO2** derived from a VOC channel, not true
  NDIR CO2, and it does not reliably track occupancy. The proposal carries a `comment`
  saying so.

The sensor on the demo rig is a Teensy 4.0 running an emulator sketch, not a production
air-quality part. See [`VERIFIED_ON_TARGET.md`](VERIFIED_ON_TARGET.md) for exactly what
that does and does not prove.

## Licensing

The `.vspec`, units and quantities files in `proposal/` derive from COVESA's Vehicle Signal
Specification and carry its **MPL-2.0** licence. Everything else here is **MIT**, like the
rest of the repository. QPP itself is Apache-2.0 and belongs to QNX; it is not vendored
here.
