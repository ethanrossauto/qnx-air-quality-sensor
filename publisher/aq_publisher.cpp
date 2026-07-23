/*
 * aq_publisher.cpp
 *
 * QNX Sensor Framework client that subscribes to the air-quality sensor unit,
 * decodes the SENSOR_FORMAT_USER_DATA payload (aq_sample_t, see the driver's
 * aq_external_sensor.h), and re-publishes it to the laptop "infotainment" HMI
 * as an HTTP Server-Sent-Events (SSE) stream on :8090.
 *
 * Why SSE: it is a plain-HTTP text stream, so a browser IVI can consume it with
 * EventSource and any other HTTP client (a mobile app, curl) just as easily, and
 * it is trivial to serve from C++ (no WebSocket handshake/framing). One TCP socket
 * per connected HMI.
 *
 * Data path:  sensor service (unit 1) -> libsensor callback here -> JSON -> SSE
 * Keep-warm:  the SSE writer emits at ~4 Hz (repeating the latest sample between
 *             1 Hz sensor updates) so the WiFi power-save never re-engages and
 *             the UI stays smooth.
 *
 * Usage:  aq_publisher [unit] [port] [bus_label]
 *         defaults: unit=1  port=8090  bus_label=sim
 *
 * Build: SDP app tree (links libsensor + socket). Deploy via qconn+curl.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sensor/sensor_api.h>
#include "aq_external_sensor.h"   /* aq_sample_t, AQ_DATA_ID, aq_status, valid flags */

/* ---- shared latest sample (written by sensor callback, read by SSE writers) ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static aq_sample_t     g_sample;
static uint32_t        g_seq       = 0;
static bool            g_haveData  = false;
static const char*     g_busLabel  = "sim";

/* ============================ sensor client side ============================ */

static const char* status_str(uint8_t st)
{
    switch (st) {
    case AQ_STATUS_ALERT: return "ALERT";
    case AQ_STATUS_WARN:  return "WARN";
    default:              return "OK";
    }
}

static const char* status_msg(uint8_t st)
{
    switch (st) {
    case AQ_STATUS_ALERT:
        return "Cabin air filter no longer protecting occupants. Service recommended.";
    case AQ_STATUS_WARN:
        return "Cabin air quality degrading.";
    default:
        return "Cabin air quality nominal.";
    }
}

/* Second alert: cabin CO2 buildup (occupants breathing on recirculation). */
static const char* co2_status_msg(uint8_t st)
{
    switch (st) {
    case AQ_STATUS_ALERT:
        return "Cabin CO2 high. Bring in fresh air.";
    case AQ_STATUS_WARN:
        return "Cabin CO2 rising.";
    default:
        return "Cabin CO2 nominal.";
    }
}

/* libsensor delivers each captured buffer here (own thread inside libsensor). */
static bool aq_data_callback(sensor_handle_t handle, sensor_buffer_t* buf, void* arg)
{
    (void)handle; (void)arg;
    if (buf == NULL || buf->data == NULL) {
        return true;
    }
    if (buf->format != SENSOR_FORMAT_USER_DATA ||
        buf->info.user_data.data_size < sizeof(aq_sample_t)) {   /* payload size, not struct size */
        return true;
    }
    /* If the framework stamped a data_id, confirm it is our payload before
     * decoding. 0 = unstamped, which we accept, so this never drops valid data. */
    if (buf->info.user_data.data_id != 0 &&
        buf->info.user_data.data_id != AQ_DATA_ID) {
        return true;
    }
    const aq_sample_t* s = (const aq_sample_t*)buf->data;

    pthread_mutex_lock(&g_lock);
    memcpy(&g_sample, s, sizeof(g_sample));
    g_seq++;
    g_haveData = true;
    pthread_mutex_unlock(&g_lock);
    return true;
}

/* Attach client buffers when the service asks us to (USER_DATA path). */
static sensor_buffer_t* g_userBufs = NULL;
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

    g_userBufs = (sensor_buffer_t*)calloc(numBufs, sizeof(sensor_buffer_t));
    if (g_userBufs == NULL) {
        return ENOMEM;
    }
    for (uint32_t i = 0; i < numBufs; i++) {
        g_userBufs[i].format = format;
        g_userBufs[i].info.user_data.data_size = bufferSize;
        g_userBufs[i].info.user_data.data_type = SENSOR_TYPE_USER_DATA;
        g_userBufs[i].data = (uint8_t*)mmap(0, bufferSize,
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
    struct timespec timeout;               /* absolute deadline: now + 10s */
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

    /* Always hand us the freshest sample. */
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
        if (err != EOK) return err;
        printf("publisher: attached %u buffers\n", g_numBufs);
    }

    err = sensor_start(handle, NULL);
    if (err != EOK) {
        fprintf(stderr, "sensor_start failed: %d\n", err);
        return err;
    }
    printf("publisher: streaming from unit %d\n", (int)unit);
    return EOK;
}

/* ============================== SSE server side ============================= */

/* Emit a numeric field as its value when its valid_mask bit is set, else JSON
 * null, so the wire honors aq_sample_t.valid_mask (the payload's contract). */
static void u_or_null(char* out, size_t cap, uint32_t mask, uint32_t bit, unsigned val)
{
    if (mask & bit) snprintf(out, cap, "%u", val);
    else            snprintf(out, cap, "null");
}
static void f_or_null(char* out, size_t cap, uint32_t mask, uint32_t bit, double val)
{
    if (mask & bit) snprintf(out, cap, "%.1f", val);
    else            snprintf(out, cap, "null");
}

/* Build one JSON line from the current sample snapshot. Returns strlen. */
static int build_json(char* out, size_t cap)
{
    aq_sample_t s;
    uint32_t seq;
    bool have;
    pthread_mutex_lock(&g_lock);
    s = g_sample; seq = g_seq; have = g_haveData;
    pthread_mutex_unlock(&g_lock);

    if (!have) {
        return snprintf(out, cap,
            "{\"seq\":0,\"status\":\"INIT\",\"alert\":false,"
            "\"message\":\"Waiting for cabin sensor.\",\"source\":\"cabin_aq\","
            "\"bus\":\"%s\"}", g_busLabel);
    }

    const char* st = status_str(s.status);
    bool alert = (s.status == AQ_STATUS_ALERT);
    const char* co2_st = status_str(s.co2_status);
    bool co2_alert = (s.co2_status == AQ_STATUS_ALERT);

    /* Honor valid_mask: a channel the sensor does not report serializes as JSON
     * null, not a fake 0. The IVI already tolerates null fields. */
    uint32_t m = s.valid_mask;
    char pm1[16], pm25[16], pm10[16], no2[16], co2[16], voc[16], temp[16], rh[16], aqi[16];
    u_or_null(pm1,  sizeof pm1,  m, AQ_VALID_PM1_0, s.pm1_0);
    u_or_null(pm25, sizeof pm25, m, AQ_VALID_PM2_5, s.pm2_5);
    u_or_null(pm10, sizeof pm10, m, AQ_VALID_PM10,  s.pm10);
    u_or_null(no2,  sizeof no2,  m, AQ_VALID_NO2,   s.no2_ppb);
    u_or_null(co2,  sizeof co2,  m, AQ_VALID_CO2,   s.co2_ppm);
    u_or_null(voc,  sizeof voc,  m, AQ_VALID_VOC,   s.voc_index);
    f_or_null(temp, sizeof temp, m, AQ_VALID_TEMP,  s.temp_c_x10 / 10.0);
    f_or_null(rh,   sizeof rh,   m, AQ_VALID_RH,    s.rh_x10 / 10.0);
    u_or_null(aqi,  sizeof aqi,  m, AQ_VALID_AQI,   s.aqi);

    return snprintf(out, cap,
        "{\"seq\":%u,\"t_us\":%" PRId64 ","
        "\"pm1_0\":%s,\"pm2_5\":%s,\"pm10\":%s,"
        "\"no2_ppb\":%s,\"co2_ppm\":%s,\"voc_index\":%s,"
        "\"temp_c\":%s,\"rh_pct\":%s,\"aqi\":%s,"
        "\"status\":\"%s\",\"alert\":%s,\"message\":\"%s\","
        "\"co2_status\":\"%s\",\"co2_alert\":%s,\"co2_message\":\"%s\","
        "\"source\":\"cabin_aq\",\"bus\":\"%s\"}",
        seq, s.sample_time_us,
        pm1, pm25, pm10,
        no2, co2, voc,
        temp, rh, aqi,
        st, alert ? "true" : "false", status_msg(s.status),
        co2_st, co2_alert ? "true" : "false", co2_status_msg(s.co2_status),
        g_busLabel);
}

static int write_all(int fd, const char* p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* One connected HMI: send SSE headers, then stream data lines at ~4 Hz. */
static void* client_thread(void* arg)
{
    int fd = (int)(intptr_t)arg;
    pthread_detach(pthread_self());

    /* Drain the HTTP request (we serve the stream on any path). */
    char req[1024];
    (void)recv(fd, req, sizeof(req) - 1, 0);

    static const char* headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    if (write_all(fd, headers, strlen(headers)) != 0) {
        close(fd); return NULL;
    }

    char json[640];
    char line[720];
    for (;;) {
        int n = build_json(json, sizeof(json));
        if (n < 0) n = 0;
        int m = snprintf(line, sizeof(line), "data: %s\n\n", json);
        if (write_all(fd, line, (size_t)m) != 0) break;
        usleep(250000);   /* 4 Hz keep-warm */
    }
    close(fd);
    return NULL;
}

static int run_server(int port)
{
    signal(SIGPIPE, SIG_IGN);   /* a disconnecting HMI must not kill us */

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(lsock); return 1;
    }
    if (listen(lsock, 8) < 0) {
        perror("listen"); close(lsock); return 1;
    }
    printf("publisher: SSE server on :%d/stream\n", port);

    for (;;) {
        int cfd = accept(lsock, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, (void*)(intptr_t)cfd) != 0) {
            close(cfd);
        }
    }
    close(lsock);
    return 0;
}

int main(int argc, char** argv)
{
    sensor_unit_t unit = (sensor_unit_t)((argc > 1) ? atoi(argv[1]) : 1);
    int           port = (argc > 2) ? atoi(argv[2]) : 8090;
    if (argc > 3) g_busLabel = argv[3];

    printf("aq_publisher: unit=%d port=%d bus=%s\n", (int)unit, port, g_busLabel);

    int err = start_sensor(unit);
    if (err != EOK) {
        fprintf(stderr, "aq_publisher: failed to start sensor (%d)\n", err);
        return 1;
    }
    return run_server(port);
}
