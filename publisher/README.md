# AQ publisher (Sensor Framework client -> HTTP SSE)

`aq_publisher.cpp` subscribes to the air-quality sensor unit via **libsensor**,
decodes the `SENSOR_FORMAT_USER_DATA` payload (`aq_sample_t`, from the driver's
`aq_external_sensor.h`), and re-publishes each sample to the laptop
"infotainment" HMI as an **HTTP Server-Sent-Events (SSE)** stream on `:8090`.

Why SSE: plain-HTTP text stream, so a browser IVI (`EventSource`) and any other
HTTP client (a mobile app, `curl`) consume the same endpoint, and it is trivial to
serve from C++ (no WebSocket handshake/framing). One TCP thread per connected HMI.
Emits at ~4 Hz (repeating the latest 1 Hz sample) to keep the WiFi link warm.

## Endpoint
`GET http://<pi>:8090/stream` -> `text/event-stream`, `Access-Control-Allow-Origin: *`
Each event: `data: <json>\n\n`. JSON schema:
```
seq,t_us, pm1_0,pm2_5,pm10, no2_ppb,co2_ppm,voc_index,
temp_c,rh_pct,aqi, status("OK"|"WARN"|"ALERT"), alert(bool), message,
co2_status("OK"|"WARN"|"ALERT"), co2_alert(bool), co2_message,
source, bus("sim"|"i2c")
```
Per-channel numeric fields (`pm*`, `no2_ppb`, `co2_ppm`, `voc_index`, `temp_c`,
`rh_pct`, `aqi`) serialize as JSON `null` when the sample's `valid_mask` bit for
that channel is clear, so a partial sensor does not report fake zeros.

Before the first sample: `{"seq":0,"status":"INIT",...}`.

## Build (aarch64le, links libsensor + libsocket)
Build tree lives in the SDP at
`<SDP>/source/source_package_sf_sensor/apps/sensor/aq_publisher/`
(mirrors `sensor_example/`). From there, with `qnxsdp-env.sh` sourced:
`make CPULIST=aarch64` -> `nto/aarch64/o.le/aq_publisher`.

## Run (on the Pi, after the sensor service is up)
`aq_publisher [unit] [port] [bus_label]`   (defaults: `1 8090 sim`)
Deploy + run via `../scripts/demo.sh deploy` then `../scripts/demo.sh publisher sim|i2c`.

## Status
**Built and verified on hardware**, on both the simulator and I2C buses. Streams
live JSON to `curl`/browser; the ALERT path fires (PM2.5 ramp -> `status:"ALERT"`).
The publisher is bus-agnostic: only the driver's data source changes between sim and I2C.
