# Cabin Air — Android IVI (Kotlin + Jetpack Compose)

Architecturally-faithful version of the infotainment HMI: a real Android app that
emulates the SDV Android infotainment guest, receiving the Pi publisher's HTTP
SSE stream and showing the cabin-air-quality screen with the filter ALERT.

> The browser IVI in `../ivi/` is the runs-today HMI. This Android app needs
> Android Studio to build/run and there is **no Android SDK/JDK on the dev PC**,
> so it has not been compiled here — it is scaffolded for you to open and run.

## What it does
- Connects to `GET http://<PI_IP>:8090/stream` (SSE) via OkHttp, parses the JSON
  (kotlinx-serialization), exposes a `StateFlow<CabinAirState>` from a ViewModel.
- Compose UI (dark, landscape): PM2.5 hero tile colored by status (green/amber/red),
  secondary cards (temp, humidity, VOC, AQI), and an ALERT banner with a
  "Recirculate" pill. Editable Pi-host field + Reconnect. Live-connection dot.
- DEMO mode with a local `FakeSource` (drifting PM2.5 + trigger ALERT) so the UI
  runs without the Pi. Sources sit behind the `AirSource` interface.

## Run (in Android Studio)
1. Open `~/Claude/qnx/ivi-android/` in Android Studio (Giraffe/Koala or newer).
2. The Gradle **wrapper jar is intentionally not committed**. Let Android Studio
   generate it on first sync, or run once from a machine with Gradle:
   `gradle wrapper --gradle-version 8.7`
   (versions: AGP 8.5.2, Kotlin 2.0.0, Compose BOM 2024.06.00, minSdk 26 / target 34.)
3. Create/boot an emulator (or plug in a device), press Run.
4. Set the **Pi host** in the top bar to the CM4's IP (default `192.168.1.10`) and
   Reconnect. The Android emulator can reach the Pi on the host LAN.
5. Make sure the Pi side is up: `../scripts/demo.sh publisher sim` (or `i2c`).

## Data contract
Same SSE JSON as the browser IVI — see `../publisher/README.md`.

## Status / caveats
- Written by an assistant without an Android toolchain to compile-test it. If your
  Android Studio ships a different Kotlin/AGP, let it bump the versions on sync.
- If Gradle complains about the Compose compiler, confirm the
  `org.jetbrains.kotlin.plugin.compose` plugin is applied (Kotlin 2.0+ requirement).
