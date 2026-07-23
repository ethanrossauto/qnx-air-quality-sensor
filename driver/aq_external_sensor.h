/*
 * aq_external_sensor.h
 *
 * Air-quality (AQ) driver for the QNX Sensor Framework "external_sensor" API.
 * Brings an air-quality sensor into the framework by riding on
 * SENSOR_FORMAT_USER_DATA (the framework has no native AQ format; it models
 * camera, LiDAR, radar, GPS, IMU, and CAN, not air quality). The payload is a
 * fixed aq_sample_t struct.
 *
 * Modelled on the SDP skeleton:
 *   ~/qnx800/source/sf-sensor-examples-0.3.0.zip ->
 *   source_package_sf_sensor/lib/sensor_drivers/external_sensors/example/
 *
 * Bus-agnostic by design: the hardware read is abstracted behind aq_bus_read().
 * A simulator backend and a real I2C read path are both implemented; add another
 * part by filling in its bus read (see AQ_BUS_* below).
 */
#ifndef __AQ_EXTERNAL_SENSOR_H__
#define __AQ_EXTERNAL_SENSOR_H__

#include "sensor/sensor_api.h"
#include "sensor/external_sensor_api.h"

#include <stdint.h>
#include <stdbool.h>

/* Default sample rate. AQ sensors are slow (~1 Hz); this is not a camera. */
#define AQ_DEFAULT_FREQUENCY_HZ     1

/* data_id / marker so a consumer can recognise our USER_DATA payload. */
#define AQ_DATA_ID                  0x41515331u  /* "AQS1" */

/*
 * Which fields in aq_sample_t are populated. Different AQ parts expose
 * different subsets (a bare PM sensor has no gas channels, etc.), so the
 * publisher/UI must check the mask rather than assume every field is valid.
 */
enum aq_valid_flags {
    AQ_VALID_PM1_0   = (1u << 0),
    AQ_VALID_PM2_5   = (1u << 1),
    AQ_VALID_PM10    = (1u << 2),
    AQ_VALID_NO2     = (1u << 3),
    AQ_VALID_CO2     = (1u << 4),
    AQ_VALID_VOC     = (1u << 5),
    AQ_VALID_TEMP    = (1u << 6),
    AQ_VALID_RH      = (1u << 7),
    AQ_VALID_AQI     = (1u << 8),
};

/* status field values. Used by BOTH alerts:
 *   status     -> particulate / cabin-filter alert (driven by PM2.5)
 *   co2_status -> cabin CO2-buildup alert (driven by CO2; the 2nd alert) */
enum aq_status {
    AQ_STATUS_OK     = 0,
    AQ_STATUS_WARN   = 1,
    AQ_STATUS_ALERT  = 2,   /* filter -> "Cabin air filter no longer protecting occupants";
                             * co2    -> "Cabin CO2 high, bring in fresh air" */
};

/*
 * The AQ data packet (the SENSOR_FORMAT_USER_DATA payload).
 * Fixed layout, little-endian, packed so on-wire size is stable across the
 * qconn/publisher boundary. Scaled integers avoid floats on the wire.
 */
#pragma pack(push, 1)
typedef struct {
    int64_t   sample_time_us;   /* sensor-time domain, microseconds */
    uint32_t  valid_mask;       /* OR of aq_valid_flags */
    uint16_t  pm1_0;            /* ug/m^3            */
    uint16_t  pm2_5;            /* ug/m^3            */
    uint16_t  pm10;            /* ug/m^3            */
    uint16_t  no2_ppb;          /* ppb               */
    uint16_t  co2_ppm;          /* ppm               */
    uint16_t  voc_index;        /* 1..500 (Sensirion-style VOC index) */
    int16_t   temp_c_x10;       /* deg C * 10        */
    uint16_t  rh_x10;           /* %RH * 10          */
    uint8_t   aqi;              /* 0..255 composite  */
    uint8_t   status;           /* enum aq_status: particulate / cabin-filter alert */
    uint8_t   co2_status;       /* enum aq_status: cabin CO2-buildup alert (2nd alert) */
} aq_sample_t;
#pragma pack(pop)

/* Which physical bus the real sensor sits on. Defaults to the simulator. */
typedef enum {
    AQ_BUS_SIM  = 0,   /* generate plausible data in software (default now) */
    AQ_BUS_I2C  = 1,   /* /dev/i2cN, 7-bit addr        (implemented)       */
    AQ_BUS_UART = 2,   /* /dev/serN, framed protocol   (TODO: implement)   */
} aq_bus_type_t;

/* Driver context (one per configured sensor unit). */
typedef struct {
    /* --- config (from parse_config / SENSOR_UNIT block) --- */
    uint32_t                     mFrequency;      /* Hz */
    aq_bus_type_t                mBusType;
    int                          mI2cBus;         /* e.g. 1 for /dev/i2c1 */
    int                          mI2cAddr;        /* 7-bit */
    char                         mUartDev[64];    /* e.g. /dev/ser1 */

    /* demo hooks: when set the simulator ramps to trip each alert independently */
    bool                         mSimPollute;   /* PM2.5 -> filter alert    */
    bool                         mSimCo2;       /* CO2   -> CO2-buildup alert */

    /* simulator state, per context so two configured units do not share one
     * simulated atmosphere (and rand_r keeps it thread-safe). */
    unsigned                     mSimRand;      /* rand_r() seed */
    int                          mSimTick;
    uint16_t                     mSimPm25;
    uint16_t                     mSimCo2Val;

    /* --- timing (mirrors the SDP example's pacing) --- */
    float                        mDataPeriodInNs;
    float                        mNsPerCycle;
    int64_t                      mSystemClockRes;
    uint64_t                     mStart;
    bool                         mFirstPacket;

    /* --- framework plumbing --- */
    sensor_streaming_params_t    mParams;
    void*                        mArg;
    sensor_external_callbacks_t  mCallback;

    /* --- opaque bus handle (fd for i2c/uart), -1 when sim/closed --- */
    int                          mBusFd;
} aqSensorContext_t;

#endif /* __AQ_EXTERNAL_SENSOR_H__ */
