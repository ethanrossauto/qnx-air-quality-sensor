/*
 * aq_external_sensor.cpp
 *
 * QNX Sensor Framework external_sensor driver for an air-quality sensor.
 * Emits SENSOR_FORMAT_USER_DATA packets of type aq_sample_t (see header).
 *
 * Bus-agnostic: the only hardware-specific code is aq_bus_read(). A software
 * simulator backend runs the whole pipeline (sensor service -> publisher -> IVI)
 * with no hardware. To wire a real sensor part, implement the AQ_BUS_I2C or
 * AQ_BUS_UART branch for that part's bus and register map.
 *
 * Build: drop this dir into the SDP Sensor Framework examples package tree at
 *   <SDP>/source/.../lib/sensor_drivers/external_sensors/aq/ (copy the Makefile
 * from the sibling example/ dir), and make with qcc (aarch64le). Deploy the .so
 * via qconn and reference it from a SENSOR_UNIT block: address = <path-to-.so>.
 *
 * Verified: compiles clean with qcc (aarch64le) and streams SENSOR_FORMAT_USER_DATA
 * on a real QNX target (Raspberry Pi CM4), on both the simulator and the I2C bus.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <devctl.h>
#include <sys/syspage.h>
#include <hw/i2c.h>

/* Required in the one file that defines external_sensor_defs, before any
 * sensor header include. */
#define EXTERNAL_SENSOR_API_IMPLEMENT

#include <sensor_utils/sensor_external_log.h>
#include "aq_external_sensor.h"

#define NANOSECONDS_PER_SECOND      1000000000L

/* ---- AQI / status thresholds (simplified PM2.5 bands, ug/m^3) ----
 * Good < 12, Moderate 12..35 (warn), Unhealthy > 35 (alert). Tune to the
 * cabin-filter use case; this is a placeholder scale. */
#define AQ_PM25_WARN_UGM3           12
#define AQ_PM25_ALERT_UGM3          35

/* ---- Cabin CO2-buildup bands (ppm): drives the 2nd alert (co2_status) ----
 * ~1000 ppm = drowsiness / focus dip onset, ~1500+ = measurable cognitive
 * impairment. Clean outdoor air ~420, an occupied closed cabin on recirc climbs
 * fast. NOTE: if a sensor reports ESTIMATED eCO2 (VOC-derived, e.g. from a
 * metal-oxide gas sensor) rather than true NDIR CO2, this alert needs a real
 * NDIR CO2 sensor (e.g. SCD40/41/30) to be trustworthy. Emulated here for the demo. */
#define AQ_CO2_WARN_PPM             1000
#define AQ_CO2_ALERT_PPM            1500

/* =====================================================================
 * Bus backends
 * ===================================================================== */

/* Simulator: plausible cabin air that drifts, and ramps to an alert when
 * mSimPollute is set (the demo "hold a marker to the sensor" moment). */
static void aq_bus_read_sim(aqSensorContext_t* ctx, aq_sample_t* s)
{
    ctx->mSimTick++;

    if (ctx->mSimPollute) {
        if (ctx->mSimPm25 < 120) ctx->mSimPm25 += 6;   /* ramp up to trip the alert */
    } else {
        /* gentle wander back toward baseline */
        int drift  = (rand_r(&ctx->mSimRand) % 3) - 1; /* -1,0,+1 */
        int target = 8 + (ctx->mSimTick % 5);
        ctx->mSimPm25 = (uint16_t)(ctx->mSimPm25 + drift +
                                   ((target > ctx->mSimPm25) ? 1 : -1));
        if (ctx->mSimPm25 < 3)  ctx->mSimPm25 = 3;
        if (ctx->mSimPm25 > 40) ctx->mSimPm25 = 40;
    }

    /* CO2 buildup (2nd alert): climbs while mSimCo2 is set (sealed cabin on
     * recirc), decays back toward ~500 when cleared. */
    if (ctx->mSimCo2) {
        if (ctx->mSimCo2Val < 3000) ctx->mSimCo2Val += 70;
    } else {
        int cdrift  = (rand_r(&ctx->mSimRand) % 3) - 1;  /* -1,0,+1 */
        int ctarget = 500 + (ctx->mSimTick % 4) * 20;    /* wanders ~500..560 */
        ctx->mSimCo2Val = (uint16_t)(ctx->mSimCo2Val + cdrift +
                                     ((ctarget > ctx->mSimCo2Val) ? 8 : -8));
        if (ctx->mSimCo2Val < 440) ctx->mSimCo2Val = 440;
        if (ctx->mSimCo2Val > 650) ctx->mSimCo2Val = 650;
    }

    uint16_t pm25 = ctx->mSimPm25;
    s->valid_mask = AQ_VALID_PM1_0 | AQ_VALID_PM2_5 | AQ_VALID_PM10 |
                    AQ_VALID_CO2 | AQ_VALID_VOC | AQ_VALID_TEMP |
                    AQ_VALID_RH | AQ_VALID_AQI;
    s->pm1_0     = (uint16_t)(pm25 * 3 / 4);
    s->pm2_5     = pm25;
    s->pm10      = (uint16_t)(pm25 + 4);
    s->no2_ppb   = 0;
    s->co2_ppm   = ctx->mSimCo2Val;
    s->voc_index = (uint16_t)(100 + (pm25 * 2));
    s->temp_c_x10 = (int16_t)(215 + (rand_r(&ctx->mSimRand) % 10));  /* ~21.5 C */
    s->rh_x10    = (uint16_t)(400 + (rand_r(&ctx->mSimRand) % 50));   /* ~40 %RH */
}

/*
 * Real I2C read. Master issues a plain 18-byte read (no register write) and the
 * slave returns the little-endian block documented in teensy_aq_i2c_slave.ino:
 *   0 pm1_0  2 pm2_5  4 pm10  6 voc  8 temp_c_x10  10 rh_x10  12 no2_ppb
 *   14 co2_ppm  16 seq  17 checksum(8-bit sum of bytes 0..16)
 * The Teensy rig emulates this; a real sensor part will define its own register
 * map, at which point only the unpack below changes.
 */
#define AQ_I2C_BLOCK_LEN 18

static uint16_t aq_le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int aq_bus_read_i2c(aqSensorContext_t* ctx, aq_sample_t* s)
{
    if (ctx->mBusFd < 0) {
        LOG_ERROR("AQ_BUS_I2C: bus not open");
        return EBADF;
    }

    /* Union gives i2c_recv_t alignment while allowing byte access to the trailing
     * data block; casting a bare uint8_t[] to i2c_recv_t* is unaligned-access UB. */
    union {
        i2c_recv_t hdr;
        uint8_t    bytes[sizeof(i2c_recv_t) + AQ_I2C_BLOCK_LEN];
    } rx;
    rx.hdr.slave.addr = (uint32_t)ctx->mI2cAddr;
    rx.hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    rx.hdr.len        = AQ_I2C_BLOCK_LEN;
    rx.hdr.stop       = 1;

    int rc = devctl(ctx->mBusFd, DCMD_I2C_RECV, &rx, sizeof(rx.bytes), NULL);
    if (rc != EOK) {
        LOG_ERROR("AQ_BUS_I2C: DCMD_I2C_RECV addr 0x%x failed: %s",
                  ctx->mI2cAddr, strerror(rc));
        return rc;
    }

    const uint8_t* d = rx.bytes + sizeof(i2c_recv_t);
    uint8_t sum = 0;
    for (int i = 0; i < AQ_I2C_BLOCK_LEN - 1; i++) sum += d[i];
    if (sum != d[AQ_I2C_BLOCK_LEN - 1]) {
        LOG_ERROR("AQ_BUS_I2C: checksum mismatch (got 0x%02x want 0x%02x)",
                  d[AQ_I2C_BLOCK_LEN - 1], sum);
        return EIO;
    }

    s->pm1_0     = aq_le16(d + 0);
    s->pm2_5     = aq_le16(d + 2);
    s->pm10      = aq_le16(d + 4);
    s->voc_index = aq_le16(d + 6);
    s->temp_c_x10 = (int16_t)aq_le16(d + 8);
    s->rh_x10    = aq_le16(d + 10);
    s->no2_ppb   = aq_le16(d + 12);
    s->co2_ppm   = aq_le16(d + 14);
    s->valid_mask = AQ_VALID_PM1_0 | AQ_VALID_PM2_5 | AQ_VALID_PM10 |
                    AQ_VALID_VOC | AQ_VALID_TEMP | AQ_VALID_RH |
                    AQ_VALID_NO2 | AQ_VALID_CO2;
    return EOK;
}

/* TODO(real sensor): real UART read. Open /dev/serN in init, read/parse the
 * part's framed protocol here, convert -> aq_sample_t. */
static int aq_bus_read_uart(aqSensorContext_t* ctx, aq_sample_t* s)
{
    (void)ctx; (void)s;
    LOG_ERROR("AQ_BUS_UART not implemented yet (need sensor part/framing)");
    return ENOTSUP;
}

static int aq_bus_read(aqSensorContext_t* ctx, aq_sample_t* s)
{
    switch (ctx->mBusType) {
    case AQ_BUS_SIM:  aq_bus_read_sim(ctx, s); return EOK;
    case AQ_BUS_I2C:  return aq_bus_read_i2c(ctx, s);
    case AQ_BUS_UART: return aq_bus_read_uart(ctx, s);
    default:          return EINVAL;
    }
}

/* Derive AQI + both alert statuses. status = particulate/filter (PM2.5);
 * co2_status = cabin CO2-buildup (CO2). The two are independent. */
static void aq_apply_status(aq_sample_t* s)
{
    s->aqi = (uint8_t)(s->pm2_5 > 255 ? 255 : s->pm2_5);   /* crude 1:1 for now */
    if ((s->valid_mask & AQ_VALID_PM2_5) == 0) {
        s->status = AQ_STATUS_OK;              /* no PM2.5 channel -> no filter alert */
    } else if (s->pm2_5 > AQ_PM25_ALERT_UGM3) {
        s->status = AQ_STATUS_ALERT;
    } else if (s->pm2_5 > AQ_PM25_WARN_UGM3) {
        s->status = AQ_STATUS_WARN;
    } else {
        s->status = AQ_STATUS_OK;
    }

    if ((s->valid_mask & AQ_VALID_CO2) == 0) {
        s->co2_status = AQ_STATUS_OK;              /* no CO2 channel -> no alert */
    } else if (s->co2_ppm > AQ_CO2_ALERT_PPM) {
        s->co2_status = AQ_STATUS_ALERT;
    } else if (s->co2_ppm > AQ_CO2_WARN_PPM) {
        s->co2_status = AQ_STATUS_WARN;
    } else {
        s->co2_status = AQ_STATUS_OK;
    }
}

/* =====================================================================
 * external_sensor_defs implementation
 * ===================================================================== */

static void* open_aq_sensor(void)
{
    sensor_external_log_init();
    aqSensorContext_t* ctx = (aqSensorContext_t*)calloc(1, sizeof(aqSensorContext_t));
    if (ctx != NULL) {
        ctx->mFrequency = AQ_DEFAULT_FREQUENCY_HZ;
        ctx->mBusType   = AQ_BUS_SIM;
        ctx->mI2cBus    = -1;
        ctx->mI2cAddr   = -1;
        ctx->mBusFd     = -1;
        ctx->mSimRand   = 0x1234u;   /* fixed seed: reproducible demo data */
        ctx->mSimTick   = 0;
        ctx->mSimPm25   = 8;         /* clean cabin baseline */
        ctx->mSimCo2Val = 500;       /* clean cabin baseline (~outdoor + a little) */
    }
    return (void*)ctx;
}

static void close_aq_sensor(void* handle)
{
    if (handle != NULL) {
        aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
        if (ctx->mBusFd >= 0) close(ctx->mBusFd);
        free(ctx);
    }
}

static int init_aq_sensor(void* handle, sensor_streaming_params_t* streamingParams)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL || streamingParams == NULL) {
        return EINVAL;
    }

    ctx->mDataPeriodInNs = (float)NANOSECONDS_PER_SECOND / (float)ctx->mFrequency;
    ctx->mNsPerCycle = (float)NANOSECONDS_PER_SECOND /
                       (float)SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    struct timespec res;
    if (clock_getres(CLOCK_REALTIME, &res) == 0) {
        ctx->mSystemClockRes = res.tv_nsec;
    } else {
        int e = errno;   /* save before LOG_ERROR/strerror can clobber it */
        LOG_ERROR("Failed to get clock resolution: %s", strerror(e));
        return e;
    }

    /* For AQ_BUS_I2C open the bus fd now (sim/uart need nothing here). */
    if (ctx->mBusType == AQ_BUS_I2C) {
        if (ctx->mI2cBus < 0 || ctx->mI2cAddr < 0) {
            LOG_ERROR("AQ_BUS_I2C requires i2c_bus and i2c_addr in config");
            return EINVAL;
        }
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/i2c%d", ctx->mI2cBus);
        ctx->mBusFd = open(dev, O_RDWR);
        if (ctx->mBusFd < 0) {
            int e = errno;   /* save before LOG_ERROR/strerror can clobber it */
            LOG_ERROR("AQ_BUS_I2C: open %s failed: %s", dev, strerror(e));
            return e;
        }
        LOG_DEBUG1("AQ_BUS_I2C: opened %s for slave 0x%x", dev, ctx->mI2cAddr);
    }

    memcpy(&ctx->mParams, streamingParams, sizeof(sensor_streaming_params_t));
    return EOK;
}

static int deinit_aq_sensor(void* handle)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (ctx != NULL && ctx->mBusFd >= 0) {
        close(ctx->mBusFd);
        ctx->mBusFd = -1;   /* so a later init re-opens instead of leaking the fd */
    }
    return EOK;
}

static int start_streaming_aq(void* handle)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL) {
        return EINVAL;
    }
    ctx->mFirstPacket = true;
    return EOK;
}

static int stop_streaming_aq(void* handle)
{
    (void)handle;
    return EOK;
}

static int64_t get_time_aq(void* handle)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL) {
        return 0;
    }
    /* microseconds, from the free-running counter (matches example). Compute in
     * double so the growing cycle count is not quantized by float's 24-bit mantissa. */
    return (int64_t)(((double)ClockCycles() * ctx->mNsPerCycle) / 1000.0);
}

/* Sleep one sample period. Called on both the success and error paths, so a
 * failing bus (e.g. an unplugged sensor returning EIO on every call) cannot spin
 * get_packet at full speed and flood the log. */
static void aq_pace(const aqSensorContext_t* ctx)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ctx->mDataPeriodInNs / NANOSECONDS_PER_SECOND);
    ts.tv_nsec = (long)ctx->mDataPeriodInNs % NANOSECONDS_PER_SECOND;
    nanosleep(&ts, NULL);
}

static int get_packet_aq(void* handle, void* bufferIn, sensor_flags_t* flags,
                         void** bufferOut, int64_t* timestamp)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL || bufferIn == NULL || flags == NULL ||
        bufferOut == NULL || timestamp == NULL) {
        return EINVAL;
    }

    flags->captured = false;

    aq_sample_t* s = (aq_sample_t*)bufferIn;
    memset(s, 0, sizeof(*s));

    int err = aq_bus_read(ctx, s);
    if (err != EOK) {
        aq_pace(ctx);   /* pace even on error so a failing bus does not spin */
        return err;
    }
    aq_apply_status(s);

    int64_t now = get_time_aq(handle);
    s->sample_time_us = now;
    s->valid_mask |= AQ_VALID_AQI;

    *bufferOut = bufferIn;
    *timestamp = now;
    flags->captured = true;
    /* 0 == "buffer completely filled". Our buffer size IS sizeof(aq_sample_t)
     * and we fill all of it, so report 0. Setting a non-zero value makes the
     * framework call setBufferFilledSize(), which it does not support for
     * SENSOR_FORMAT_USER_DATA (type 21) and fails buffer prep -> 100% dropped. */
    flags->numFilledBytes = 0;

    aq_pace(ctx);   /* pace to the configured frequency (AQ is slow, ~1 Hz) */
    return EOK;
}

static int get_buffer_requirements_aq(void* handle, uint32_t* numBuffers,
                                      uint32_t* bufSize)
{
    if (handle == NULL || numBuffers == NULL || bufSize == NULL) {
        return EINVAL;
    }
    *numBuffers = 1;
    *bufSize    = sizeof(aq_sample_t);
    return EOK;
}

static int fill_format_info_aq(void* handle, sensor_format_info_t* info,
                               uint32_t* infoSize)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL || info == NULL || infoSize == NULL) {
        return EINVAL;
    }
    if (ctx->mParams.out_data_format != SENSOR_FORMAT_USER_DATA) {
        LOG_ERROR("AQ driver only supports SENSOR_FORMAT_USER_DATA (got %d)",
                  ctx->mParams.out_data_format);
        return EINVAL;
    }
    /* For SENSOR_FORMAT_USER_DATA the framework interprets info as the
     * user_data union member (NOT the generic .data). Stamp our marker so a
     * consumer can recognise the aq_sample_t payload. publish_timestamp is
     * filled by the framework at publish time. */
    info->user_data.data_size = sizeof(aq_sample_t);
    info->user_data.data_id   = AQ_DATA_ID;
    info->user_data.data_type = (sensor_type_t)SENSOR_TYPE_USER_DATA;
    *infoSize = sizeof(info->user_data);
    return EOK;
}

/* The framework passes config values verbatim after '=', including surrounding
 * whitespace (e.g. "bus = sim" arrives as " sim"). Trim both ends in place so
 * string comparisons below are robust to normal config formatting. */
static char* aq_trim(char* s)
{
    if (s == NULL) return s;
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static int parse_config_aq(void* handle, char* name, char* value)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL || name == NULL || value == NULL) {
        return EINVAL;
    }

    name  = aq_trim(name);
    value = aq_trim(value);

    if (strcmp(name, "frequency") == 0) {
        long f = strtol(value, NULL, 10);
        if (f <= 0) { LOG_ERROR("frequency must be > 0"); return EINVAL; }
        ctx->mFrequency = (uint32_t)f;
    } else if (strcmp(name, "bus") == 0) {
        if      (strcmp(value, "sim")  == 0) ctx->mBusType = AQ_BUS_SIM;
        else if (strcmp(value, "i2c")  == 0) ctx->mBusType = AQ_BUS_I2C;
        else if (strcmp(value, "uart") == 0) ctx->mBusType = AQ_BUS_UART;
        else { LOG_ERROR("unknown bus '%s'", value); return EINVAL; }
    } else if (strcmp(name, "i2c_bus") == 0) {
        ctx->mI2cBus = (int)strtol(value, NULL, 10);
    } else if (strcmp(name, "i2c_addr") == 0) {
        ctx->mI2cAddr = (int)strtol(value, NULL, 0);   /* allow 0x.. */
    } else if (strcmp(name, "uart_dev") == 0) {
        strncpy(ctx->mUartDev, value, sizeof(ctx->mUartDev) - 1);
    } else if (strcmp(name, "sim_pollute") == 0) {
        ctx->mSimPollute = (strtol(value, NULL, 10) != 0);
    } else if (strcmp(name, "sim_co2") == 0) {
        ctx->mSimCo2 = (strtol(value, NULL, 10) != 0);
    } else {
        return ENOTSUP;   /* not one of ours; framework may handle it */
    }
    return EOK;
}

static int set_callbacks_aq(void* handle, void* arg,
                            sensor_external_callbacks_t* callback)
{
    aqSensorContext_t* ctx = (aqSensorContext_t*)handle;
    if (handle == NULL || arg == NULL || callback == NULL) {
        return EINVAL;
    }
    ctx->mArg = arg;
    memcpy(&ctx->mCallback, callback, sizeof(sensor_external_callbacks_t));
    return EOK;
}

/* The framework loads this symbol (no tag = single driver in the .so). */
sensor_external_sensor_t external_sensor_defs = {
    open : open_aq_sensor,
    close : close_aq_sensor,
    init : init_aq_sensor,
    deinit : deinit_aq_sensor,
    start_streaming : start_streaming_aq,
    stop_streaming : stop_streaming_aq,
    get_packet : get_packet_aq,
    get_buffer_requirements : get_buffer_requirements_aq,
    get_time : get_time_aq,
    parse_config : parse_config_aq,
    set_sensor_metadata : NULL,
    get_metadata_limits : NULL,
    fill_format_info : fill_format_info_aq,
    set_callbacks : set_callbacks_aq,
    set_calibration_params : NULL,
    perform_calibration : NULL
};
