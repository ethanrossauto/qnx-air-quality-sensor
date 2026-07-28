/*
 * aq_signal_publisher.c -- publish an I2C air-quality sensor straight into the
 * QNX signal service as COVESA-VSS-named signals.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Ross
 *
 * Data path:
 *   sensor on /dev/i2cN  ->  this process  ->  /dev/qpp/Vehicle/.../<Signal>
 *
 * There is deliberately no QNX Sensor Framework in that path. The Sensor
 * Framework is built for high-bandwidth streaming devices (camera, lidar,
 * radar); a ~1 Hz air-quality sensor is a signal source, so it belongs on the
 * signal service directly. A separate program, aq_signal_connector.c, bridges
 * the Sensor Framework to QPP instead -- keep that one if the sensor is already
 * exposed as a framework unit, use this one otherwise.
 *
 * Publishing is plain POSIX: open("/dev/qpp/Vehicle/Cabin/AirQuality/PM25",
 * O_WRONLY) and write() the 4-byte float that the catalog's "datatype": "float"
 * leaf expects. That is the whole QPP client API. Each write is a whole-value
 * update: the fd is opened once and written every cycle with no lseek, and the
 * value read back is always the last one written (verified on target). File
 * position matters on the read side, where a consumer must lseek back to 0 to
 * re-read, but not here.
 *
 * Run qpp first with the air-quality catalog, then this:
 *   qpp -s -c qpp_catalog_airquality_demo.json -m /dev/qpp
 *   aq_signal_publisher -b 1 -a 0x28 [-m /dev/qpp] [-f 1]
 *
 * Read a signal back with any POSIX tool (quote the path so the shell leaves
 * the '?' alone):
 *   cat '/dev/qpp/Vehicle/Cabin/AirQuality/PM25?text'
 *   od -t f4 -N 4 /dev/qpp/Vehicle/Cabin/AirQuality/PM25
 *
 * Wire format (18 bytes, little-endian, read in one DCMD_I2C_RECV):
 *   0  pm1_0 u16 ug/m^3      8  temp_c_x10 i16 degC*10
 *   2  pm2_5 u16 ug/m^3     10  rh_x10     u16 %RH*10
 *   4  pm10  u16 ug/m^3     12  no2_ppb    u16 ppb
 *   6  voc_index u16 (1..500, dimensionless index)
 *  14  co2_ppm u16 ppm      16  seq u8     17  checksum u8 (sum of bytes 0..16)
 *
 * PORTING TO YOUR OWN SENSOR
 * --------------------------
 * Replace aq_i2c_read(). It is the only place the wire format lives; everything
 * downstream works off the decoded aq_reading_t. Two things to get right:
 *
 *   1. Many real parts need a write-then-read (DCMD_I2C_SENDRECV) to select a
 *      register, a start-measurement command at open time, and per-word CRC-8
 *      rather than one trailing sum byte. That all belongs in aq_i2c_read() and
 *      aq_i2c_open(); nothing above them changes.
 *   2. Set out->valid_mask to just the channels your part actually measures.
 *      Signals whose bit is clear are never written, so a PM-only sensor leaves
 *      CO2 and NO2 untouched instead of publishing a fake 0.0. Getting this
 *      wrong is the difference between "no reading" and "a reading of zero".
 *
 * To publish a different set of signals, add a row to g_signals[] below. The
 * table carries the VSS path, the valid bit, and where to find the value in
 * aq_reading_t, so one row is genuinely all it takes. The signal must also
 * exist in the catalog qpp booted from: either regenerate it with
 * ../catalog/make_qpp_catalog.py, or just hand-edit the JSON, which is a plain
 * vss-tools export and is meant to be readable.
 *
 * Deliberate scope choices, to stay honest to the VSS mapping:
 *   - voc_index is a Sensirion-style INDEX (dimensionless), not a ppb
 *     concentration, so by default it is NOT written to .../AirQuality/TVOC
 *     (unit: ppb). Build with -DAQ_VOC_FIELD_IS_PPB if the attached part
 *     reports true TVOC ppb in that field.
 *   - Cabin relative humidity is read off the wire but not published: the VSS
 *     standard catalog has Exterior.Humidity only, and there is no correct home
 *     for a cabin reading. Candidate for a follow-up proposal.
 *   - Air-quality index and alert thresholds are derived policy, not
 *     measurements. Alert policy (e.g. actuating
 *     Cabin/HVAC/IsRecirculationActive) belongs in a policy component, not in a
 *     data publisher.
 *
 * Known limitations, stated plainly:
 *   - One sensor, one I2C bus address, fixed 18-byte block.
 *   - Nothing in this path carries a timestamp. QPP signal metadata holds the
 *     datatype, description, unit and range, not a last-updated time, and this
 *     program does not add one. A consumer cannot tell a fresh value from a
 *     stale one by reading the signal alone. If that matters, publish a
 *     companion signal carrying the sample time.
 *   - If QPP is restarted underneath this process, cached signal fds go stale;
 *     a failed write drops the fd and the next cycle re-opens it, so it
 *     recovers within one sample period. Signals that never open at startup are
 *     retried the same way.
 *   - If the I2C device stops answering, reads are retried every cycle and the
 *     bus fd is re-opened after AQ_REOPEN_AFTER consecutive failures, forever.
 *     Stale values are left in QPP rather than zeroed.
 *   - No GPIO pin muxing is done here. On boards whose image does not mux the
 *     I2C pins by default, mux them before starting this program.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <hw/i2c.h>

/* ------------------------------- wire format ------------------------------ */

#define AQ_I2C_BLOCK_LEN   18
#define AQ_REOPEN_AFTER    5     /* consecutive read failures before re-open */

/* Which channels a reading actually carries. Deliberately defined here rather
 * than included from the Sensor Framework driver: this program does not link
 * against the framework at all. */
enum {
    AQ_VALID_PM1  = (1u << 0),
    AQ_VALID_PM25 = (1u << 1),
    AQ_VALID_PM10 = (1u << 2),
    AQ_VALID_NO2  = (1u << 3),
    AQ_VALID_CO2  = (1u << 4),
    AQ_VALID_VOC  = (1u << 5),
    AQ_VALID_TEMP = (1u << 6),
    AQ_VALID_RH   = (1u << 7),
};

typedef struct {
    uint32_t valid_mask;   /* OR of the AQ_VALID_* bits above */
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm10;
    uint16_t voc_index;
    int16_t  temp_c_x10;
    uint16_t rh_x10;
    uint16_t no2_ppb;
    uint16_t co2_ppm;
    uint8_t  seq;
} aq_reading_t;

static uint16_t aq_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ------------------------------ signal table ------------------------------
 * One row per published signal: where it lives in the VSS tree, which valid
 * bit gates it, and where to read it out of aq_reading_t. Adding a signal is
 * one row (plus a catalog entry). */

typedef enum { FLD_U16, FLD_I16 } aq_field_t;

typedef struct {
    const char *rel_path;    /* signal path under the QPP mount point */
    uint32_t    valid_bit;   /* gate: skip the write when this bit is clear */
    size_t      offset;      /* byte offset of the field in aq_reading_t */
    aq_field_t  type;        /* how to read it */
    float       scale;       /* multiply by this to reach the VSS unit */
    int         fd;          /* cached open fd, -1 when closed */
    bool        warned;      /* an error was already logged for this signal */
} aq_signal_t;

static aq_signal_t g_signals[] = {
    { "Vehicle/Cabin/AirQuality/PM1",  AQ_VALID_PM1,  offsetof(aq_reading_t, pm1_0),      FLD_U16, 1.0f, -1, false },
    { "Vehicle/Cabin/AirQuality/PM25", AQ_VALID_PM25, offsetof(aq_reading_t, pm2_5),      FLD_U16, 1.0f, -1, false },
    { "Vehicle/Cabin/AirQuality/PM10", AQ_VALID_PM10, offsetof(aq_reading_t, pm10),       FLD_U16, 1.0f, -1, false },
    { "Vehicle/Cabin/AirQuality/CO2",  AQ_VALID_CO2,  offsetof(aq_reading_t, co2_ppm),    FLD_U16, 1.0f, -1, false },
    { "Vehicle/Cabin/AirQuality/NO2",  AQ_VALID_NO2,  offsetof(aq_reading_t, no2_ppb),    FLD_U16, 1.0f, -1, false },
#ifdef AQ_VOC_FIELD_IS_PPB
    { "Vehicle/Cabin/AirQuality/TVOC", AQ_VALID_VOC,  offsetof(aq_reading_t, voc_index),  FLD_U16, 1.0f, -1, false },
#endif
    { "Vehicle/Cabin/HVAC/AmbientAirTemperature",
                                       AQ_VALID_TEMP, offsetof(aq_reading_t, temp_c_x10), FLD_I16, 0.1f, -1, false },
};

#define SIG_COUNT ((int)(sizeof(g_signals) / sizeof(g_signals[0])))

static const char *g_mount = "/dev/qpp";
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static float field_value(const aq_reading_t *r, const aq_signal_t *sig)
{
    const void *p = (const uint8_t *)r + sig->offset;
    float raw;
    if (sig->type == FLD_I16) {
        int16_t v;
        memcpy(&v, p, sizeof(v));
        raw = (float)v;
    } else {
        uint16_t v;
        memcpy(&v, p, sizeof(v));
        raw = (float)v;
    }
    return raw * sig->scale;
}

/* Open (or re-open) one signal value file under the mount point. Logs the first
 * failure per signal only, so a missing catalog entry does not flood the log
 * once per sample forever. */
static int signal_fd(aq_signal_t *sig)
{
    if (sig->fd >= 0) {
        return sig->fd;
    }
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", g_mount, sig->rel_path);
    if (n <= 0 || n >= (int)sizeof(path)) {
        if (!sig->warned) {
            fprintf(stderr, "signal path too long: %s/%s\n", g_mount, sig->rel_path);
            sig->warned = true;
        }
        return -1;
    }
    sig->fd = open(path, O_WRONLY);
    if (sig->fd == -1) {
        if (!sig->warned) {
            fprintf(stderr, "open(%s) failed: %s (further errors on this signal suppressed)\n",
                    path, strerror(errno));
            sig->warned = true;
        }
    } else if (sig->warned) {
        fprintf(stderr, "%s/%s recovered\n", g_mount, sig->rel_path);
        sig->warned = false;
    }
    return sig->fd;
}

/* Publish one float signal. Drops the cached fd on error so the next cycle
 * re-opens it (e.g. if qpp restarted underneath us). */
static void publish_float(aq_signal_t *sig, float value)
{
    int fd = signal_fd(sig);
    if (fd < 0) {
        return;
    }
    if (write(fd, &value, sizeof(value)) != (ssize_t)sizeof(value)) {
        if (!sig->warned) {
            fprintf(stderr, "write(%s/%s) failed: %s (further errors on this signal suppressed)\n",
                    g_mount, sig->rel_path, strerror(errno));
            sig->warned = true;
        }
        close(sig->fd);
        sig->fd = -1;
    }
}

/* Publish every signal whose channel the sensor actually reported. A channel
 * the sensor does not measure is left alone, not written as zero. */
static void publish_reading(const aq_reading_t *r)
{
    for (int i = 0; i < SIG_COUNT; i++) {
        if (r->valid_mask & g_signals[i].valid_bit) {
            publish_float(&g_signals[i], field_value(r, &g_signals[i]));
        }
    }
}

static void close_signals(void)
{
    for (int i = 0; i < SIG_COUNT; i++) {
        if (g_signals[i].fd >= 0) {
            close(g_signals[i].fd);
            g_signals[i].fd = -1;
        }
    }
}

/* --------------------------------- the bus -------------------------------- */

static int aq_i2c_open(int bus, bool quiet)
{
    char dev[32];
    int n = snprintf(dev, sizeof(dev), "/dev/i2c%d", bus);
    if (n <= 0 || n >= (int)sizeof(dev)) {
        return -1;
    }
    int fd = open(dev, O_RDWR);
    if (fd == -1 && !quiet) {
        fprintf(stderr, "open(%s) failed: %s\n", dev, strerror(errno));
    }
    return fd;
}

/*
 * One block read from the sensor. Same DCMD_I2C_RECV shape the Sensor Framework
 * driver uses on this board: the union gives i2c_recv_t alignment while still
 * allowing byte access to the trailing data block (casting a bare uint8_t[] to
 * i2c_recv_t* is unaligned-access UB).
 *
 * Errors are returned, not logged, so the caller can log on transition instead
 * of once per failed sample.
 */
static int aq_i2c_read(int fd, int addr, aq_reading_t *out)
{
    union {
        i2c_recv_t hdr;
        uint8_t    bytes[sizeof(i2c_recv_t) + AQ_I2C_BLOCK_LEN];
    } rx;

    memset(&rx, 0, sizeof(rx));
    rx.hdr.slave.addr = (uint32_t)addr;
    rx.hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    rx.hdr.len        = AQ_I2C_BLOCK_LEN;
    rx.hdr.stop       = 1;

    int rc = devctl(fd, DCMD_I2C_RECV, &rx, sizeof(rx.bytes), NULL);
    if (rc != EOK) {
        return rc;
    }

    const uint8_t *d = rx.bytes + sizeof(i2c_recv_t);
    uint8_t sum = 0;
    for (int i = 0; i < AQ_I2C_BLOCK_LEN - 1; i++) {
        sum = (uint8_t)(sum + d[i]);
    }
    if (sum != d[AQ_I2C_BLOCK_LEN - 1]) {
        return EIO;
    }

    out->pm1_0      = aq_le16(d + 0);
    out->pm2_5      = aq_le16(d + 2);
    out->pm10       = aq_le16(d + 4);
    out->voc_index  = aq_le16(d + 6);
    out->temp_c_x10 = (int16_t)aq_le16(d + 8);
    out->rh_x10     = aq_le16(d + 10);
    out->no2_ppb    = aq_le16(d + 12);
    out->co2_ppm    = aq_le16(d + 14);
    out->seq        = d[16];

    /* This emulator reports every channel. A real part should set only the bits
     * it actually measures -- see PORTING above. */
    out->valid_mask = AQ_VALID_PM1  | AQ_VALID_PM25 | AQ_VALID_PM10 |
                      AQ_VALID_VOC  | AQ_VALID_TEMP | AQ_VALID_RH   |
                      AQ_VALID_NO2  | AQ_VALID_CO2;
    return EOK;
}

/* ----------------------------- argument parsing --------------------------- */

/* strtol with the checks atoi() skips: no digits, trailing junk, out of range. */
static bool parse_int(const char *s, int min, int max, int *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 0);   /* base 0 so -a 0x28 works */
    if (errno != 0 || end == s || *end != '\0' || v < (long)min || v > (long)max) {
        return false;
    }
    *out = (int)v;
    return true;
}

static void usage(const char *argv0)
{
    printf("Usage: %s -b <i2c bus> -a <7-bit addr> [-m <qpp mount>] [-f <Hz>] [-v]\n"
           "  -b  I2C bus number, e.g. 1 for /dev/i2c1        (required)\n"
           "  -a  7-bit slave address, e.g. 0x28              (required)\n"
           "  -m  QPP mount point                             (default /dev/qpp)\n"
           "  -f  sample rate in Hz, 1..100                   (default 1)\n"
           "  -v  print each reading to stdout\n",
           argv0);
}

int main(int argc, char *argv[])
{
    int  bus     = -1;
    int  addr    = -1;
    int  freq_hz = 1;
    bool verbose = false;
    int  opt;

    while ((opt = getopt(argc, argv, "hvb:a:m:f:")) != -1) {
        switch (opt) {
            case 'b':
                if (!parse_int(optarg, 0, 99, &bus)) {
                    fprintf(stderr, "bad -b '%s' (want 0..99)\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'a':
                if (!parse_int(optarg, 0x08, 0x77, &addr)) {
                    fprintf(stderr, "bad -a '%s' (want a 7-bit address, 0x08..0x77)\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'f':
                if (!parse_int(optarg, 1, 100, &freq_hz)) {
                    fprintf(stderr, "bad -f '%s' (want 1..100)\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'm': g_mount = optarg; break;
            case 'v': verbose = true;   break;
            case 'h': usage(argv[0]);   return EXIT_SUCCESS;
            default:  usage(argv[0]);   return EXIT_FAILURE;
        }
    }
    if (bus < 0 || addr < 0) {
        fprintf(stderr, "-b and -a are both required\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (access(g_mount, F_OK | X_OK) != 0) {
        fprintf(stderr, "QPP mount %s not accessible (%s) -- is qpp running with the catalog?\n",
                g_mount, strerror(errno));
        return EXIT_FAILURE;
    }

    int bus_fd = aq_i2c_open(bus, false);
    if (bus_fd < 0) {
        return EXIT_FAILURE;
    }

    /* Terminate the loop on Ctrl-C / slay so the fds get closed properly. No
     * SA_RESTART, so clock_nanosleep breaks out on EINTR instead of resuming. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("publisher: /dev/i2c%d @0x%02x -> %s at %d Hz\n", bus, addr, g_mount, freq_hz);
    fflush(stdout);

    const long period_ns = 1000000000L / freq_hz;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    int  fails       = 0;
    bool bus_warned  = false;   /* log bus trouble on transition, not per sample */

    while (g_running) {
        aq_reading_t r;
        memset(&r, 0, sizeof(r));

        int rc = aq_i2c_read(bus_fd, addr, &r);
        if (rc == EOK) {
            if (bus_warned) {
                fprintf(stderr, "/dev/i2c%d @0x%02x recovered\n", bus, addr);
                bus_warned = false;
            }
            fails = 0;
            publish_reading(&r);
            if (verbose) {
                printf("seq=%u PM1=%u PM2.5=%u PM10=%u CO2=%u NO2=%u TEMP=%.1f RH=%.1f\n",
                       r.seq, r.pm1_0, r.pm2_5, r.pm10, r.co2_ppm, r.no2_ppb,
                       r.temp_c_x10 / 10.0f, r.rh_x10 / 10.0f);
                fflush(stdout);
            }
        } else {
            if (!bus_warned) {
                fprintf(stderr, "read from /dev/i2c%d @0x%02x failed: %s "
                                "(further errors suppressed until it recovers)\n",
                        bus, addr, strerror(rc));
                bus_warned = true;
            }
            if (++fails >= AQ_REOPEN_AFTER) {
                /* Bus wedged or device gone: re-open and keep trying. Never give
                 * up -- the i2c resource manager may itself be restarting. */
                close(bus_fd);
                bus_fd = aq_i2c_open(bus, true);
                fails  = 0;
            }
        }

        /* Absolute deadline so a slow read does not make the rate drift. If a
         * stall put the deadline in the past, skip ahead rather than running a
         * burst of back-to-back cycles to catch up. */
        struct timespec now;
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec  += 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (next.tv_sec < now.tv_sec ||
            (next.tv_sec == now.tv_sec && next.tv_nsec < now.tv_nsec)) {
            next = now;
            next.tv_nsec += period_ns;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec  += 1;
            }
        }
        if (bus_fd < 0) {
            /* Cannot read anything until the bus comes back; still pace the
             * retries rather than spinning. */
            bus_fd = aq_i2c_open(bus, true);
        }
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != EOK && !g_running) {
            break;
        }
    }

    printf("publisher: shutting down\n");
    close_signals();
    if (bus_fd >= 0) {
        close(bus_fd);
    }
    return EXIT_SUCCESS;
}
