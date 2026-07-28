/*
 * aq_signal_connector.c -- bridge an existing QNX Sensor Framework unit into
 * the QNX signal service as COVESA-VSS-named signals.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Ross
 *
 * Subscribes to the air-quality unit in the QNX Sensor Framework (the same way
 * publisher/aq_publisher.cpp does), decodes the SENSOR_FORMAT_USER_DATA payload
 * (aq_sample_t), and publishes each valid channel into QPP, the QNX POSIX
 * Publish Subscribe resource manager
 * (github.com/qnx/qnx-posix-publish-subscribe).
 *
 * USE THIS ONE if the sensor is already exposed as a Sensor Framework unit.
 * If you are starting from a bare I2C device, aq_signal_publisher.c in this
 * folder is the better shape -- it talks to the bus directly and leaves the
 * Sensor Framework out of the path entirely.
 *
 * Signal writes are plain POSIX: open("/dev/qpp/Vehicle/Cabin/AirQuality/PM25",
 * O_WRONLY) and write() the binary value (float, 4 bytes, matching the
 * "datatype": "float" leaves in the catalog). That is the entire QPP client
 * API -- see signal-framework/sample/sample-signal-publisher in the QPP repo.
 *
 * Run qpp first with the demo catalog from ../catalog/:
 *   qpp -s -c qpp_catalog_airquality_demo.json -m /dev/qpp
 * then:
 *   aq_signal_connector [-m /dev/qpp] [-u 1]
 *
 * Data path:
 *   aq driver -> sensor service (USER_DATA) -> this connector -> QPP signal files
 *   Consumers: cat /dev/qpp/Vehicle/Cabin/AirQuality/PM25?text   (or poll() the
 *   binary file, or hang an Android-VHAL / IVI Signal Adaptor off it).
 *
 * Build: see the Makefile in this folder. qpp itself must be built from the
 * QNX GitHub repo; it does not ship in the SDP.
 *
 * Deliberate scope choices, to stay honest to the VSS mapping:
 *   - voc_index is a Sensirion-style INDEX (1..500, dimensionless), not a ppb
 *     concentration, so by default it is NOT written to .../AirQuality/TVOC
 *     (unit: ppb). If the deployed sensor reports true TVOC ppb in that field,
 *     build with -DAQ_VOC_FIELD_IS_PPB.
 *   - rh_x10 (cabin relative humidity) has no home in the VSS standard catalog
 *     (VSS has Exterior.Humidity only; verified 2026-07-27), so it is not
 *     published. Candidate for a follow-up VSS proposal or a custom leaf.
 *   - aqi, status, co2_status are derived/policy values, not measurements;
 *     alert policy (e.g. actuating Cabin/HVAC/IsRecirculationActive) belongs in
 *     a policy component, not in a data connector.
 *
 * KNOWN LIMITATIONS (stated plainly, because they are real):
 *   - If the sensor service is restarted underneath this process, the libsensor
 *     handle binds to the dead instance and published values freeze at their
 *     last value. There is no re-subscribe; restart the connector. Observed on
 *     target, not theoretical.
 *   - No clean shutdown: the main loop never exits, so the sensor handle and
 *     the attached buffers are released only by process teardown.
 *   - A failed signal write drops the cached fd, so QPP restarts are survived.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <sensor/sensor_api.h>
#include "aq_external_sensor.h"   /* aq_sample_t, AQ_DATA_ID, valid flags */

/* ------------------------------ signal table ------------------------------ */

typedef struct {
    const char *rel_path;      /* signal path under the QPP mount point */
    uint32_t    valid_bit;     /* aq_sample_t.valid_mask gate */
    int         fd;            /* cached open fd, -1 when closed */
} aq_signal_t;

enum {
    SIG_PM1 = 0, SIG_PM25, SIG_PM10, SIG_CO2, SIG_NO2, SIG_TVOC, SIG_TEMP,
    SIG_COUNT
};

static aq_signal_t g_signals[SIG_COUNT] = {
    [SIG_PM1]  = { "Vehicle/Cabin/AirQuality/PM1",  AQ_VALID_PM1_0, -1 },
    [SIG_PM25] = { "Vehicle/Cabin/AirQuality/PM25", AQ_VALID_PM2_5, -1 },
    [SIG_PM10] = { "Vehicle/Cabin/AirQuality/PM10", AQ_VALID_PM10,  -1 },
    [SIG_CO2]  = { "Vehicle/Cabin/AirQuality/CO2",  AQ_VALID_CO2,   -1 },
    [SIG_NO2]  = { "Vehicle/Cabin/AirQuality/NO2",  AQ_VALID_NO2,   -1 },
    [SIG_TVOC] = { "Vehicle/Cabin/AirQuality/TVOC", AQ_VALID_VOC,   -1 },
    [SIG_TEMP] = { "Vehicle/Cabin/HVAC/AmbientAirTemperature", AQ_VALID_TEMP, -1 },
};

static const char *g_mount = "/dev/qpp";

/* Open (or re-open) one signal value file under the mount point. */
static int signal_fd(aq_signal_t *sig)
{
    if (sig->fd >= 0) {
        return sig->fd;
    }
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", g_mount, sig->rel_path);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return -1;
    }
    sig->fd = open(path, O_WRONLY);
    if (sig->fd == -1) {
        fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
    }
    return sig->fd;
}

/* Publish one float signal; drops the cached fd on error so we re-open next time
 * (e.g. if qpp restarted underneath us). */
static void publish_float(aq_signal_t *sig, float value)
{
    int fd = signal_fd(sig);
    if (fd < 0) {
        return;
    }
    if (write(fd, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        fprintf(stderr, "write(%s/%s) failed: %s\n", g_mount, sig->rel_path, strerror(errno));
        close(sig->fd);
        sig->fd = -1;
    }
}

/* ---------------------------- sensor client side ---------------------------
 * Same USER_DATA subscription pattern as publisher/aq_publisher.cpp (which is
 * proven on hardware); the only difference is what we do with each sample. */

static bool aq_data_callback(sensor_handle_t handle, sensor_buffer_t *buf, void *arg)
{
    (void)handle; (void)arg;
    if (buf == NULL || buf->data == NULL) {
        return true;
    }
    if (buf->format != SENSOR_FORMAT_USER_DATA ||
        buf->info.user_data.data_size < sizeof(aq_sample_t)) {
        return true;
    }
    /* Accepting data_id == 0 is a deliberate compatibility allowance for units
     * that never stamp the field, but it is loose: any USER_DATA source with an
     * unset data_id gets reinterpreted as an aq_sample_t. Safe on a single-unit
     * demo; tighten to an exact AQ_DATA_ID match if the system carries more
     * than one USER_DATA sensor. */
    if (buf->info.user_data.data_id != 0 &&
        buf->info.user_data.data_id != AQ_DATA_ID) {
        return true;
    }
    const aq_sample_t *s = (const aq_sample_t *)buf->data;
    const uint32_t     m = s->valid_mask;

    if (m & AQ_VALID_PM1_0) publish_float(&g_signals[SIG_PM1],  (float)s->pm1_0);
    if (m & AQ_VALID_PM2_5) publish_float(&g_signals[SIG_PM25], (float)s->pm2_5);
    if (m & AQ_VALID_PM10)  publish_float(&g_signals[SIG_PM10], (float)s->pm10);
    if (m & AQ_VALID_CO2)   publish_float(&g_signals[SIG_CO2],  (float)s->co2_ppm);
    if (m & AQ_VALID_NO2)   publish_float(&g_signals[SIG_NO2],  (float)s->no2_ppb);
#ifdef AQ_VOC_FIELD_IS_PPB
    if (m & AQ_VALID_VOC)   publish_float(&g_signals[SIG_TVOC], (float)s->voc_index);
#endif
    if (m & AQ_VALID_TEMP)  publish_float(&g_signals[SIG_TEMP], (float)s->temp_c_x10 / 10.0f);
    return true;
}

static sensor_buffer_t *g_userBufs = NULL;
static uint32_t         g_numBufs  = 0;

static int attach_buffers(sensor_handle_t handle)
{
    sensor_format_t format;
    uint64_t bufferSize;
    uint32_t numBufs;
    int err = sensor_get_streaming_property(handle,
                                            SENSOR_STREAMPROP_FORMAT, &format,
                                            SENSOR_STREAMPROP_BUFFER_SIZE, &bufferSize,
                                            SENSOR_STREAMPROP_BUFFERS_NUM, &numBufs);
    if (err != EOK) {
        fprintf(stderr, "get_streaming_property failed: %d\n", err);
        return err;
    }
    if (format != SENSOR_FORMAT_USER_DATA) {
        fprintf(stderr, "unexpected format %d (want USER_DATA)\n", format);
        return EINVAL;
    }
    g_userBufs = (sensor_buffer_t *)calloc(numBufs, sizeof(sensor_buffer_t));
    if (g_userBufs == NULL) {
        return ENOMEM;
    }
    for (uint32_t i = 0; i < numBufs; i++) {
        g_userBufs[i].format = format;
        g_userBufs[i].info.user_data.data_size = bufferSize;
        g_userBufs[i].info.user_data.data_type = SENSOR_TYPE_USER_DATA;
        g_userBufs[i].data = (uint8_t *)mmap(0, bufferSize,
                                             PROT_READ | PROT_WRITE | PROT_NOCACHE,
                                             MAP_ANON | MAP_PHYS | MAP_SHARED, NOFD, 0);
        if (g_userBufs[i].data == MAP_FAILED || g_userBufs[i].data == NULL) {
            fprintf(stderr, "mmap buffer %u failed\n", i);
            return ENOMEM;
        }
    }
    g_numBufs = numBufs;
    err = sensor_attach_buffers(handle, g_userBufs, numBufs);
    if (err != EOK) {
        fprintf(stderr, "sensor_attach_buffers failed: %d\n", err);
    }
    return err;
}

static int start_sensor(sensor_unit_t unit)
{
    struct timespec timeout;
    clock_gettime(CLOCK_MONOTONIC, &timeout);
    timeout.tv_sec += 10;
    int err = sensor_wait_for_available(unit, &timeout);
    if (err != EOK) {
        fprintf(stderr, "unit %d not available: %d\n", (int)unit, err);
        return err;
    }

    sensor_handle_t handle = NULL;
    err = sensor_open(unit, SENSOR_ACCESSMODE_DATA, &handle);
    if (err != EOK || handle == NULL) {
        fprintf(stderr, "sensor_open failed: %d\n", err);
        return err ? err : EIO;
    }
    sensor_set_buffer_retrieval_mode(handle, SENSOR_BRM_LATEST_FLUSH);

    err = sensor_register_data_callback(handle, SENSOR_EVENT_STREAM_DATA,
                                        SENSOR_EVENTMODE_READONLY,
                                        aq_data_callback, NULL);
    if (err != EOK) {
        fprintf(stderr, "register_data_callback failed: %d\n", err);
        return err;
    }

    sensor_buffer_alloc_status_t allocStatus;
    err = sensor_get_buffer_alloc_status(handle, &allocStatus);
    if (err == EOK && allocStatus == SENSOR_BUFFER_ALLOC_STATUS_NEED_ATTACH) {
        err = attach_buffers(handle);
        if (err != EOK) {
            return err;
        }
        printf("connector: attached %u buffers\n", g_numBufs);
    }

    err = sensor_start(handle, NULL);
    if (err != EOK) {
        fprintf(stderr, "sensor_start failed: %d\n", err);
        return err;
    }
    printf("connector: streaming from unit %d into %s\n", (int)unit, g_mount);
    return EOK;
}

int main(int argc, char *argv[])
{
    sensor_unit_t unit = 1;
    int opt;
    while ((opt = getopt(argc, argv, "hm:u:")) != -1) {
        switch (opt) {
            case 'm': g_mount = optarg; break;
            case 'u': {
                /* strtol, not atoi: reject empty input, trailing junk and
                 * out-of-range values rather than silently reading them as 0. */
                errno = 0;
                char *end = NULL;
                long v = strtol(optarg, &end, 0);
                if (errno != 0 || end == optarg || *end != '\0' || v < 0 || v > 255) {
                    fprintf(stderr, "bad -u '%s' (want a sensor unit, 0..255)\n", optarg);
                    return EXIT_FAILURE;
                }
                unit = (sensor_unit_t)v;
                break;
            }
            case 'h':
                printf("Usage: %s [-m <qpp mount, default /dev/qpp>] [-u <sensor unit, default 1>]\n",
                       argv[0]);
                return EXIT_SUCCESS;
            default:
                return EXIT_FAILURE;
        }
    }

    if (access(g_mount, F_OK | X_OK) != 0) {
        fprintf(stderr, "QPP mount %s not accessible (%s) -- is qpp running with the catalog?\n",
                g_mount, strerror(errno));
        return EXIT_FAILURE;
    }

    int err = start_sensor(unit);
    if (err != EOK) {
        return EXIT_FAILURE;
    }

    /* libsensor delivers data on its own thread; nothing to do here. */
    for (;;) {
        (void)sleep(60);
    }
    return EXIT_SUCCESS;
}
