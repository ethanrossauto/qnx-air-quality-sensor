# Validation record: air-quality VSS proposal

_Run 2026-07-27. Everything below was actually executed; the commands are reproducible.
The scratch inputs (a VSS master checkout and a Python venv) were not kept, so re-running
needs a fresh VSS download; the commands show the shape._

## Toolchain

- VSS standard catalog: `COVESA/vehicle_signal_specification` **master** (tarball downloaded
  2026-07-27; master was at version 6.0-dev, post-v6.0 which was released 2026-01-16).
- vss-tools: **7.0.0.dev0 installed from git master**. This is what VSS CI itself uses on
  master (per `scripts/install_vss_tools.sh` in the VSS repo, which pip-installs
  `git+https://github.com/COVESA/vss-tools@master`).
  - Note: released vss-tools 6.0.0 from PyPI FAILS on today's *unmodified* master (the new
    `Vehicle.Exterior.RoadSurfaceCondition` etc. use an `enum` extra attribute that 6.0.0's
    `--strict` rejects). That failure is upstream's, not the proposal's; use vss-tools master.

## Run 1: baseline sanity (unmodified master)

```
vspec export json -u ./spec/units.yaml -q ./spec/quantities.yaml --strict \
  -s ./spec/VehicleSignalSpecification.vspec -o baseline.json
```
Result: **exit 0**, 1367 leaf signals.

## Run 2: patched tree (the PR-ready form)

Applied `../proposal/patches/0001-add-air-quality-signals.patch` to a copy of master
(adds `spec/include/AirComposition.vspec`, edits `spec/Cabin/Cabin.vspec`,
`spec/Vehicle/Exterior.vspec`, `spec/units.yaml`, `spec/quantities.yaml`), then:

```
vspec export json -u ./spec/units.yaml -q ./spec/quantities.yaml --strict \
  -s ./spec/VehicleSignalSpecification.vspec -o vss_full_with_airquality.json --pretty
vspec export yaml -u ./spec/units.yaml -q ./spec/quantities.yaml --strict \
  -s ./spec/VehicleSignalSpecification.vspec -o vss_full_with_airquality.yaml
```
Result: **both exit 0, zero errors/criticals**, 1381 leaf signals = baseline + 14
(7 signals under `Vehicle.Cabin.AirQuality` + 7 under `Vehicle.Exterior.AirQuality`).
The exports themselves are build output and are not checked in; the commands above
regenerate them.

## Run 3: overlay form against PRISTINE master (usable today, no fork)

```
vspec export json \
  -u ./spec/units.yaml      -u <this>/proposal/overlay/units_airquality.yaml \
  -q ./spec/quantities.yaml -q <this>/proposal/overlay/quantities_airquality.yaml \
  --strict \
  -s ./spec/VehicleSignalSpecification.vspec \
  -l <this>/proposal/overlay/airquality_overlay.vspec \
  -o overlay_out.json
```
Result: **exit 0**; both AirQuality branches present with all 7 signals each.

## Run 4: QPP catalog generation + QNX builds

- `catalog/make_qpp_catalog.py` pruned Run 3's export to the 17-signal demo catalog
  `catalog/qpp_catalog_airquality_demo.json` (root `Vehicle` branch verified, every leaf
  has `type` + `datatype`, matching what `qpp/catalog.c` in the QPP repo requires).
- QPP (github.com/qnx/qnx-posix-publish-subscribe, main, QPP_VERSION 1.1.0) built with
  the SDP environment sourced and `make`: **exit 0**, produced `qpp` for x86_64 and
  aarch64le.
- `connector/aq_signal_connector.c` compiled and linked against the Sensor Framework
  (`make connector`): **exit 0, no warnings**.
- `connector/aq_signal_publisher.c` compiled standalone (`make publisher`), SDP headers
  only, no Sensor Framework: **exit 0, no warnings** under `-Wall -Wextra`.

## What was NOT validated (needs hardware / upstream)

- ~~Running qpp + the catalog + the connector on target~~ **DONE, see
  `VERIFIED_ON_TARGET.md`**, which records both the bridged path and the direct
  I²C-to-signal-service path running on a Raspberry Pi CM4.
- The full VSS CI matrix (`make mandatory_targets` = ~14 exporters incl. protobuf, ddsidl,
  apigear...). Only the json and yaml exporters were run, in `--strict` mode. CI also
  runs pre-commit hooks (yamllint etc.) not run here.
- QPP behavior with JSON comments or unknown keys in the catalog (the generated catalog is
  clean JSON with only conventional keys, so this should not matter).
