# COVESA VSS proposal: air-quality signals (Cabin + Exterior)

_Drafted 2026-07-27. This document is the design rationale; the actual artifacts are the
files next to it. Everything here follows conventions read from the real VSS repo
(master), not invented ones. Validation record: `../VALIDATION.md`._

## What is proposed

Fourteen new signals (the same seven measurements at two measurement points), plus the
three concentration units and one quantity the catalog is missing:

```
Vehicle.Cabin.AirQuality.{PM1, PM25, PM10, CO2, NO2, Ozone, TVOC}
Vehicle.Exterior.AirQuality.{PM1, PM25, PM10, CO2, NO2, Ozone, TVOC}
units:      ppm, ppb, ug/m^3      quantities: mass-per-volume
```

All `type: sensor`, `datatype: float`, `min: 0`, units per signal (µg/m³ for PM, ppm for
CO2, ppb for NO2/O3/TVOC).

## The artifacts

| File | What it is |
|---|---|
| `files/AirComposition.vspec` | The new spec file as it would land in-tree at `spec/include/AirComposition.vspec`: one shared definition included from both Cabin and Exterior with a branch prefix, mirroring how `SingleHVACStation.vspec` and `include/MovableItem.vspec` are reused today. |
| `patches/0001-add-air-quality-signals.patch` | The complete PR as a unified diff against master (5 files: the new include, `Cabin/Cabin.vspec`, `Vehicle/Exterior.vspec`, `units.yaml`, `quantities.yaml`). Applies with `patch -p1` in a VSS checkout; validated with VSS CI's own toolchain. |
| `overlay/airquality_overlay.vspec` + `overlay/units_airquality.yaml` + `overlay/quantities_airquality.yaml` | The same signals as a standard VSS **overlay**, usable today against an unmodified catalog. This is the form the QNX demo in this repository consumes, and it needs no COVESA involvement to use. |

## Design decisions and their justifications

### Branch placement: `Cabin.AirQuality` and `Exterior.AirQuality`, via a shared include

- **Not under `Cabin.HVAC`**: HVAC in VSS is the climate *actuation* system (recirculation,
  defrost, AC, fan, station temperature). Air composition is a property of the cabin
  environment that exists whether or not HVAC acts on it. Precedent: interior sound level
  would go on the cabin, not on the radio.
- **Not only Cabin**: `Vehicle.Exterior` already models ambient environment
  (AirTemperature, Humidity, AirPressure, wind, visibility, precipitation) and the
  strongest automotive use case (automatic recirculation when driving into pollution)
  needs the *outside* reading. A shared `include/AirComposition.vspec` keeps the two
  branches identical by construction, the exact pattern VSS uses for repeated structures.
- A third placement option (a new top-level `Vehicle.AirQuality`) was rejected: VSS
  separates interior (`Cabin`) from ambient (`Exterior`) consistently, and a maintainer
  would likely bounce a proposal that muddles that.

### Naming

- `PM1` / `PM25` / `PM10`: VSS names are PascalCase alphanumerics, with **no underscores or
  dots anywhere in the current catalog** (verified by grep), so `PM2_5`/`PM2.5` are out.
  `PM25` follows the widespread flattening used by, e.g., air-quality APIs and the Matter
  standard's field naming. The description spells out "PM2.5" and the micrometer cutoff,
  so ambiguity with PM10/PM1 is handled where VSS handles it everywhere else, in the
  description. (Chemical-formula style names have precedent: `EmissionsCO2`, `HVAC`, `ABS`.)
- `CO2`, `NO2`, `Ozone`, `TVOC`: formulas where the formula is the common name, the word
  where the word is (nobody says "O3 alert"). `TVOC` (total VOC, ppb) chosen over a
  Sensirion-style "VOCIndex" because an index is vendor-algorithm-specific; a concentration
  is standardizable.
- `AirQuality` as the branch name: a plain PascalCase noun phrase, consistent with VSS
  branch naming, and QNX's own signal catalog already carries a `Cabin/HVAC/Purifier`
  branch in the same neighbourhood.

### Datatypes, min/max

- `float` everywhere: matches how VSS models physical measurements (`Exterior.Humidity`,
  `AirTemperature` are floats), and real sensors report sub-integer resolution.
- `min: 0` (a concentration cannot be negative); **no max**: VSS generally omits max where
  no physical/protocol bound exists (wildfire PM2.5 readings exceed 1000 µg/m³; a max
  invites clipping). `Exterior.Humidity` has max 100 because percent has one; ppm/ppb/µg/m³
  do not.

### The eCO2 problem (honesty note that must survive into the PR)

Many low-cost air-quality parts do not measure CO2 at all: they report **estimated eCO2
derived from a VOC channel, not true NDIR CO2**. The two behave differently: eCO2 tracks
volatile organics, so it does not reliably track occupancy buildup, which is the main
reason anyone wants cabin CO2 in the first place. The proposed `CO2` signal therefore
carries a `comment` acknowledging that some sensors report eCO2; VSS uses `comment` for
exactly this kind of implementation nuance. If COVESA reviewers prefer, a split (`CO2` vs
`CO2Estimated`) is a reasonable review outcome; starting with one signal plus a comment is
the smaller ask.

### New units and quantity (this is the structurally biggest part of the ask)

VSS today has **no concentration units at all**, so the proposal must touch
`units.yaml`/`quantities.yaml`, which is a wider-radius change than adding leaves (units
are shared catalog infrastructure). Definitions follow the file's exact house style, with
QUDT cross-references (all five URIs verified resolvable 2026-07-27):

- `ppm`, `ppb` → quantity `relation` (dimensionless ratio, same bucket as `percent`),
  QUDT `unit/PPM`, `unit/PPB`, quantity-kind `DimensionlessRatio`.
- `ug/m^3` → new quantity `mass-per-volume` (name styled after existing `mass-per-time`,
  `mass-per-distance`), QUDT `unit/MicroGM-PER-M3`, quantity-kind `MassDensity`; quantity
  definition cites ISO 80000-9 (mass concentration), matching the ISO-citation style of
  the existing quantities file.

### Deliberately excluded (and why that strengthens the proposal)

- **AQI/AQHI**: composite indices are jurisdiction-specific (US AQI 0-500, Canadian AQHI
  1-10+, EU CAQI...) and derivable from the raw signals. A standards body will not bless
  one index; proposing raw measurements only is the defensible scope. (The demo can still
  compute and display AQI locally, and a private catalog can carry a custom leaf.)
- **Cabin relative humidity**: genuinely missing from VSS (only `Exterior.Humidity`
  exists) and the sensor used here measures it, but it is a comfort/HVAC quantity, not air
  *composition*; bundling it would invite "wrong branch" review noise. Better as a
  follow-up one-liner PR (`Cabin.HVAC.Humidity`, where QNX's own catalog already put it).
- **CO (carbon monoxide)**: defensible for garages and tunnels, but nothing on the rig this
  proposal came from measures it, and no widely shipped cabin AQ module includes it.
  Reviewers can extend the include file trivially later. That is what the shared-include
  shape is for.

**An asymmetry to own rather than hide:** the reasoning above leans on "propose what real
hardware measures", and the Exterior branch does not meet that bar as cleanly as the Cabin
one. Nothing on the reference rig measures outside air at all, and `Exterior.AirQuality.CO2`
in particular has no obvious automotive use: ambient CO2 sits near 420 ppm everywhere, so
the signal would be near-constant. It is proposed anyway because the shared include is what
keeps the two branches identical by construction, and because the exterior *particulate* and
*NO2* readings are the ones the recirculation use case actually needs. If reviewers would
rather trim the exterior branch to the signals with a real use case, that is a reasonable
outcome and the include makes it a small change. Dropping the shared include to hand-pick
per-branch signals is the thing worth resisting, not the trimming itself.

## Anticipated review pushback

1. "Does any production vehicle expose these?" Cabin PM sensors ship today (several
  OEMs' AQS/PM2.5 displays, air-purifier trims in Asian markets especially); the honest
  answer is "the hardware exists in production, the signal standard does not."
2. "Why floats for ppb?" Resolution, and consistency with every physical sensor in VSS.
3. Units-file conservatism. The units addition is the part most likely to get bikeshedded
  (naming: `ug/m^3` vs `ug/m3`; the caret form matches existing `m/s^2`, `cm/s^2`).
4. eCO2 vs CO2 split, as above.
5. **"Why is TVOC in ppb rather than µg/m³?"** This is the weakest unit choice in the
  proposal and deserves a straight answer. Total VOC is a mixture, so converting it to a
  mole ratio requires assuming a single molar mass that the mixture does not have. The
  reference methods (ISO 16000-6, and the WHO and AgBB guidance built on it) report TVOC as
  a mass concentration. Against that, ppb is what the sensor market actually emits, and
  µg/m³ would force every integrator to apply a vendor-specific conversion factor before
  publishing. Both defaults are defensible; ppb was chosen for the shorter path from a real
  part to a correct signal. If reviewers prefer the standards-aligned unit, switching TVOC
  to `ug/m^3` costs one line, since that unit is already part of this proposal.
6. **"Is the Exterior branch justified?"** See the asymmetry note above; the honest answer
  is that Cabin is the stronger half.
