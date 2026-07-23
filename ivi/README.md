# Cabin Air IVI (browser infotainment mock)

`index.html` is a single-file car-infotainment screen that consumes the Pi
publisher's SSE stream and shows the cabin-air-quality demo: a PM2.5 gauge that
turns green/amber/red by status, secondary readouts (temp, humidity, VOC, Cabin CO2),
and two ALERT banners that slide in on status, a cabin-filter alert driven by PM2.5
and a cabin CO2-buildup alert driven by CO2.

Self-contained (inline CSS/JS), no build step, no dependencies.

## Run
- `../scripts/demo.sh ivi` serves it on `http://localhost:8080` (recommended), or
- just open `index.html` in a browser.

Set the **Pi** field in the top bar to the Pi's IP (default `192.168.1.10`) and
click Connect. The publisher must be running on the Pi (`demo.sh publisher`).
The connection dot goes green when data is flowing.

## Demo without the Pi
Click **Demo** to run a local simulator (drifting PM2.5). While in demo mode the
same button toggles **Pollute / Clear** to force the filter alert, and a **CO2 ▲**
button forces the CO2 alert. Click Connect to leave demo mode and reconnect to the Pi.

## Data contract
Consumes the publisher's SSE JSON (see ../publisher/README.md). It is tolerant of
missing fields and shows "Waiting for cabin sensor…" until the first sample.

Note: opening via `file://` also works (the publisher sends
`Access-Control-Allow-Origin: *`), but serving over `http://localhost:8080`
avoids browser file-origin quirks.
