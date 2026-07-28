# The QNX signal service: what it actually is

_Researched 2026-07-27 from primary sources: the open-source repo itself (code read, not
just docs), the QNX blog, and the COVESA VSS repo. Claims are labeled verified or
unverified._

**Why this document exists.** The QNX Sensor Framework is built for high-bandwidth
streaming devices: cameras, LiDAR, radar. A ~1 Hz air-quality sensor is a different
animal: it produces a handful of scalars per second, and what matters is that consumers
can find them by name and know their units. That is what the **signal service** is for.
This is a write-up of what the signal service actually is, how it differs from the Sensor
Framework, and what state the relevant COVESA standard is in.

## TL;DR

1. The signal service is real, open source (Apache 2.0), and it IS COVESA-VSS-based.
   **Verified**: it boots from a "Signal Catalog defined by COVESA VSS v3" and mirrors the
   VSS tree as a filesystem.
2. The natural next question, "is there an existing VSS signal that maps to what an
   air-quality sensor delivers?", has a clear answer: **no**. The COVESA VSS standard
   catalog contains **zero air-quality signals** (no PM, CO2, VOC, NO2, ozone, or AQI) and
   **zero concentration units** (no ppm, ppb, or µg/m³). Verified by grepping the full spec
   trees of master, v6.0 and v5.1, and searching the repo's issues and PRs (details below).
3. That gap is not a wall for the QNX side: QNX's own reference catalog freely adds
   non-standard signals (`Vehicle/QNXCustom/*`, `Cabin/HVAC/Purifier/*`), so an
   air-quality branch works in the signal service **today** with a custom catalog. The only
   thing that needs COVESA is making the signals *standard*.

## What the signal service is

**QNX POSIX Publish Subscribe (QPP)**, open-sourced by QNX in 2026 under Apache 2.0.

- Repo: <https://github.com/qnx/qnx-posix-publish-subscribe> (repo title: "QNX Signal
  Framework"; `version.mk`: QPP_VERSION 1.1.0). Verified: full source downloaded and read.
- Announcement: <https://qnx.software/en/blog/2026/qnx-open-sources-the-posix-publish-subscribe-project>
- Contributions: the repo notes that PRs "can be created but there is currently no
  guarantee that they will be reviewed."

QPP is a QNX **filesystem resource manager**: signals are files, and the entire client API
is POSIX (`open`/`read`/`write`/`poll`, or `cat`/`echo` from a shell). It has two modes:

1. **Signal Service mode** (`qpp -s -c catalog.json -m /dev/qpp`): reads a Signal Catalog
   ("defined by COVESA VSS v3" per the README) and materializes Value, Control, and
   Metadata files in a directory tree mirroring the VSS paths. `-s`/`--static` locks the
   tree to the catalog.
2. **Generic pub/sub mode** (no catalog): clients create/remove topics dynamically with
   ordinary file operations.

### The client API (verified from `signal-framework/sample/` source)

- **Publish**: `open("/dev/qpp/Vehicle/Cabin/HVAC/Purifier/CleaningCycleTime", O_WRONLY)`
  and `write()` the raw binary value. Datatype-faithful: a `boolean` is 1 byte, a `float`
  4 bytes, arrays are elements contiguous in memory, no delimiters. Each write is a
  whole-value update rather than an append: a publisher can hold one fd open and write to
  it every cycle without seeking, and readers see the latest value. Verified on target
  across thousands of samples.
- **Consume**: `open(..., O_RDONLY)`, `read()`; use `poll()` on multiple signal fds to wait
  for updates; `lseek` back to 0 to re-read. File-position semantics are real on the read
  side, which is the asymmetry to remember: readers seek, writers do not.
- **No timestamps.** A signal's metadata carries its datatype, description, unit and range,
  not a last-updated time. Nothing tells a consumer whether a value arrived a millisecond
  or an hour ago. If freshness matters, a publisher has to carry it in a companion signal.
- **Human/text mode**: append `?text` to the path: `echo 28 > .../Temperature?text`,
  `cat .../Temperature?text`. (From `qpp.use` and `ioread_text.c`/`iowrite_text.c`.)
- **Actuation**: actuator signals get a control channel. A client opens `<path>?control`
  and writes a request; the component that owns the actuator watches the control file and
  applies the change. On disk these are hidden siblings (`.Name.control`, `.Name.metadata`
  in `qpp/node.c`).
- **Datatypes** (`qpp/value.c`): boolean, int8..64, uint8..64, float, double, string,
  binary, plus vector forms of all, a superset of what VSS leaves use.

### The catalog format (verified from `qpp/catalog.c` + the shipped reference catalog)

A JSON object whose single root key is the VSS root branch (`"Vehicle"`), each node with
`type` (branch/sensor/actuator/attribute), branches with `children`, leaves with
`datatype` and optional `default`. Other keys (description, unit, min/max, uuid) ride
along as metadata. **This is exactly the vss-tools JSON export format**, so
`vspec export json` output feeds QPP directly. The shipped reference catalog
(`signal-framework/signal-service/etc/signal_catalog.json`) is "based on the COVESA VSS
3.0 reference catalog" with QNX's own additions, including entirely custom branches like
`Vehicle/QNXCustom/IsAndroidAvailable` and `Cabin/HVAC/Purifier/*`. **Precedent: QNX
itself does not wait for COVESA to add signals it needs.**

### The surrounding "Signal Framework" (verified from `signal-framework/README.md`)

- **Signal Connectors** bind data sources to the service: connect to the source (CAN,
  SOME/IP, a local device, simulation/playback), normalize to the VSS form, publish into
  QPP, and service actuation requests. An `echo-actuator` sample connector ships in-repo.
  → In this vocabulary, the air-quality piece in this repository is **a Signal Connector**.
  Both shapes are provided: one whose source is the Sensor Framework AQ unit, and one that
  reads the I²C sensor directly.
- **Signal Adaptors** re-expose signals to other worlds (e.g. Android VHAL for an IVI).
  The browser and Android IVIs in this repository map onto this cleanly.

## Signal service vs Sensor Framework: the actual difference

| | Sensor Framework | Signal service (QPP) |
|---|---|---|
| Designed for | High-bandwidth streaming devices: camera, LiDAR, radar, GPS/IMU/CAN (`sensor/external_sensor_api.h`) | Discrete "points of data", one value per signal path |
| Data model | Typed buffers (frames/point clouds); no AQ format, hence the `SENSOR_FORMAT_USER_DATA` workaround | COVESA VSS catalog; named, typed scalar signals with units/metadata |
| Transport | Shared buffer pools, mmap'd, callback-driven | Resource manager + native message passing; POSIX file API |
| Consumer contract | Custom: consumer must know `aq_sample_t`'s packed layout and `AQ_DATA_ID` | Standard: any client can `cat` a signal; names/types come from the catalog |
| AQ sensor fit | Works (proven on the CM4) but is a square peg: 30 bytes/s in a framework built for MB/s | Natural: ~1 Hz scalars is exactly the shape it models |

The honest reading: the Sensor Framework driver in this repository is not wasted work. It
is a real QNX driver against a real hardware bus, and the framework genuinely transports
the data. But the *data-modeling* objection holds. `SENSOR_FORMAT_USER_DATA` means every
consumer needs a private struct definition, whereas the signal service gives every consumer
a self-describing, VSS-named tree that `cat` can read.

The two also compose. `driver → Sensor Framework → connector → QPP` is a legitimate
architecture, and it is what `connector/aq_signal_connector.c` implements. The leaner
shape reads the I²C sensor directly and skips the Sensor Framework, which is what
`connector/aq_signal_publisher.c` does. Both are provided because both are useful: use the
bridge when the sensor is already a framework unit, use the direct publisher when starting
from a bare device.

## Is it really "based on the COVESA specification"? Yes, with a version caveat

Verified: the README says the Signal Catalog is "defined by COVESA VSS v3", the reference
catalog is derived from VSS 3.0, and the parser consumes vss-tools-style JSON. Caveat:
VSS is now at v6.0 (Jan 2026); QPP's parser only requires `type`/`children`/`datatype`/
`default`, all of which are unchanged in v6.0 JSON exports, so a modern catalog loads.
"v3" reflects when the reference catalog was frozen, not a hard version pin. Confirmed in
practice: the catalog in `../catalog/`, generated from vss-tools master, loaded and served
correctly on a real qpp build (see `../VERIFIED_ON_TARGET.md`).

## The COVESA air-quality gap (the independent verification)

Checked 2026-07-27 in `COVESA/vehicle_signal_specification`:

- **master** (post-v6.0): grep of all `spec/**/*.vspec` for
  `air quality|CO2|carbon dioxide|particulate|PM1/2.5/10|VOC|NO2|ozone|AQI|IAQ` finds only
  `Vehicle.EmissionsCO2` (tailpipe g/km attribute) and
  `Powertrain.CombustionEngine.DieselParticulateFilter.*` (exhaust DPF temperatures and
  delta-pressure). Nothing about cabin or ambient air composition.
- **v6.0 and v5.1 release tags**: same greps, zero hits.
- **units**: `spec/units.yaml` has no ppm, no ppb, no µg/m³, and no concentration units at
  all. `spec/quantities.yaml` has no concentration/mass-density quantity.
- **Issues/PRs**: GitHub search of the repo for "air quality", "CO2/particulate/PM2.5/AQI",
  and "VOC/ozone/NO2/pollutant". No open proposals; the only historical CO2 item is
  closed issue #393 (about the *emissions* attribute).
- Cabin.HVAC has only actuator/comfort signals (recirculation, defrosters, AC, fan,
  temperature); `Vehicle.Exterior` has weather (temp/humidity/pressure/wind/visibility)
  but no composition.

**Conclusion: nothing to map to, and nobody else is currently proposing it.** The field is
clear for the proposal in `../proposal/`, and the interim path (custom catalog in QPP) is
fully unblocked.

## Verified vs unverified: summary

Verified from primary sources: everything above about QPP internals (source read), the
builds (see `../VALIDATION.md`), the VSS gap (greps and searches), and the VSS contribution
process (`CONTRIBUTING.md` plus the governance docs).

Unverified / open:
- Whether QNX's **commercial** SDP ships a supported signal-service package beyond the
  GitHub repo. An SDP 8.0.5 install contains no `qpp` binary; the official qnx.com SDP docs
  may say more but sit partly behind the myQNX login.
- ~~Runtime behavior on target~~ RESOLVED: QPP, the air-quality catalog and both
  connectors ran on a Raspberry Pi CM4 (see `../VERIFIED_ON_TARGET.md`). This also settles
  the "v3 vs v6" caveat above in practice.
- Whether "signal service" in QNX's own usage means exactly this open-source QPP or an
  internal productized variant. The public evidence (repo title "QNX Signal Framework",
  the blog, the VSS-catalog design) makes QPP the obvious referent.
