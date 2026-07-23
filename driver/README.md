# AQ external_sensor driver

Brings an air-quality sensor into the QNX Sensor Framework. The framework has no
native air-quality format (it models camera, LiDAR, radar, GPS, IMU, and CAN), so
AQ rides on `SENSOR_FORMAT_USER_DATA` with a fixed `aq_sample_t` payload. That is
the framework's supported path for a sensor type it does not model natively.

## Files
- `aq_external_sensor.h`: `aq_sample_t` payload, driver context, and bus enum.
- `aq_external_sensor.cpp`: the `external_sensor_defs` implementation. Bus-agnostic:
  only `aq_bus_read()` is hardware-specific. A software simulator backend is built in
  (drifts clean cabin air; `sim_pollute=1` ramps PM2.5 to trip the alert), and a real
  I2C read path (`aq_bus_read_i2c`) reads a little-endian block over `/dev/i2cN`.
- `teensy_aq_i2c_slave/`: a Teensy I2C-slave sketch that emulates a sensor for the
  I2C path, so it can be exercised on real hardware with a stand-in part.

Verified: compiles clean with `qcc` (aarch64le) into `libaq_external_sensor.so`,
deployed to a CM4, and the QNX sensor service streams `SENSOR_FORMAT_USER_DATA`
packets (`aq_sample_t`) at 1 Hz, on both the simulator and the I2C bus.

## Design decisions
- Payload is a packed `aq_sample_t` (scaled ints, little-endian) with a `valid_mask`,
  so different parts (bare PM vs PM+gas+RH/T) populate only the fields they have.
- The `status` field (OK/WARN/ALERT) drives the IVI cabin-filter card; thresholds are
  placeholder PM2.5 bands, tune to the target sensor.
- Slow rate by design (`AQ_DEFAULT_FREQUENCY_HZ = 1`); AQ is not a camera.

## Build
1. Copy this dir into the SDP Sensor Framework examples package tree, alongside the
   stock `example/` external sensor:
   `<SDP>/source/.../lib/sensor_drivers/external_sensors/aq/`.
   Copy the `Makefile` from the sibling `example/` dir into `aq/` unchanged. The parent
   `Makefile` uses QNX recursive make, so it finds `aq/` on its own; no parent edit needed.
2. Build with the QNX recursive make (`qcc`, aarch64le) into `libaq_external_sensor.so`.
3. Deploy the `.so` to the target (the repo's `scripts/demo.sh deploy` does this over
   qconn) and reference it from a `SENSOR_UNIT` block (`address = <path-to-.so>`).

## Config (SENSOR_UNIT block)
Ready-made configs are in `../configs/`. A minimal simulator unit:

```
begin SENSOR_GLOBAL
end SENSOR_GLOBAL
begin SENSOR_UNIT_1
    type = external_sensor
    name = cabin_aq
    address = /data/var/tmp/aq/libaq_external_sensor.so   # no tag = untagged external_sensor_defs
    data_format = SENSOR_FORMAT_USER_DATA
    frequency = 1
    bus = sim                                             # sim | i2c | uart
    sim_pollute = 0                                       # 1 = ramp PM2.5 to the filter alert (demo)
    sim_co2 = 0                                           # 1 = ramp CO2 to the CO2 alert (demo)
    # i2c_bus  = 1                                        # when bus = i2c
    # i2c_addr = 0x28
end SENSOR_UNIT_1
```

## Run / verify
```
# on the target (scripts/demo.sh drives these over qconn):
sensor -U 1000 -r /data/var/tmp/aq/roll -c /data/var/tmp/aq/aq.conf -vvvv &
sensor_example -u 1 -d -s        # prints live aq_sample_t bytes; alert on pollute
```

## Wiring a real sensor (moving off the simulator)
Implement `aq_bus_read` for the part's bus; the I2C branch is a worked example. You need:
1. The sensor's part number and datasheet.
2. Its bus: I2C or UART. If I2C, the bus/address and the measurement-read register
   sequence. If UART, the baud and frame format.
3. The channels it reports (PM1.0/2.5/10, NO2, CO2, VOC index, RH/T), which set the
   `AQ_VALID_*` bits and fields.
4. Its supply/logic voltage (3.3 V vs 5 V) for wiring to the Pi header.

On the `sim` bus the whole pipeline runs end to end with none of the above.
