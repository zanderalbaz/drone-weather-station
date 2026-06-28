#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* -------  TriSonica reader thread  -------------------------------- */
#include <pthread.h>
#include <dji_platform.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <math.h>

#include "logger.h"           /* shared structs, externs */
#include <dji_logger.h>
#include "widget/test_widget.h"

/* ───── UI bits you already use ───── */
_Atomic(uint8_t) g_triDisplayMode = 0;
static const char *const g_hzlabel[SAMPLE_RATE_COUNT] = {
    "5 hz", "10 hz", "20 hz", "40 hz"
};

/* from test_fc_subscription.c */
extern _Atomic(uint8_t) g_sampleRateMode;
extern bool simulate_fc;

/* ───── one global sample produced here, consumed by fusion ───── */
_Atomic TriData g_tri = { .S = NAN };

/* ───── desired-vs-active output rate mode tracking ───── */
static _Atomic uint8_t s_desired_mode = 0;   /* what the UI wants */
static uint8_t         s_active_mode  = 0;   /* what the head is set to now */

/* ───── keep the serial fd inside this module ───── */
static int s_fd = -1;

/* ===================== small helpers ===================== */

static void tri_sleep_ms(unsigned ms)
{
    usleep((useconds_t)ms * 1000U);
}

/* append CRLF, drain, and allow a brief settle */
static int tri_write_cmd(int fd, const char *fmt, ...)
{
    char cmd[96];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);

    /* append CRLF so the device latches the command */
    size_t n = strnlen(cmd, sizeof cmd);
    if (n + 2 < sizeof cmd) { cmd[n++] = '\r'; cmd[n++] = '\n'; cmd[n] = '\0'; }

    ssize_t w = write(fd, cmd, n);
    if (w < 0) return -1;

    /* ensure bytes reached the wire */
    tcdrain(fd);
    /* small settle — device firmware is fast, 150–300 ms is plenty */
    tri_sleep_ms(250);
    return 0;
}

/* map our mode → Hz value */
static inline unsigned mode_to_hz(uint8_t mode)
{
    static const unsigned hz[SAMPLE_RATE_COUNT] = {5,10,20,40};
    if (mode >= SAMPLE_RATE_COUNT) mode = 0;
    return hz[mode];
}

/* set 115200-8N1 raw, no flow control */
static void tri_config_port(int fd)
{
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tcgetattr(fd, &tio);

    cfsetspeed(&tio, B115200);
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    /* no HW flow control */
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
    /* no SW flow control */
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);

    tcsetattr(fd, TCSANOW, &tio);
}

/* switch fd to nonblocking */
static void tri_set_nonblock(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    if (on) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    else    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

/* very small line reader that tolerates non-blocking fd */
static int read_line(int fd, char *buf, size_t maxlen)
{
    size_t n = 0;
    char c;
    while (n < maxlen - 1) {
        int r = read(fd, &c, 1);
        if (r == 1) {
            if (c == '\n') break;
            if (c == '\r') continue;
            buf[n++] = c;
        } else if (r == 0) {
            /* no data */
            tri_sleep_ms(1);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                tri_sleep_ms(1);
                continue;
            }
            /* real error */
            return -1;
        }
    }
    buf[n] = '\0';
    return (int)n;
}

/* tokenize "key value key value ..." and fill struct (-99.xx -> NaN) */
static bool parse_tri(const char *line, TriData *t)
{
    memset(t, 0, sizeof(*t));
    tri_set_measurements_nan(t);

    /* very light sanity: require at least one space and one known tag */
    if (!strpbrk(line, " \t") ||
        !(strstr(line, " T ") || strstr(line, " H ") || strstr(line, " S ") || strstr(line, " P "))) {
        return false;
    }

    char copy[256];
    strncpy(copy, line, sizeof copy);
    copy[sizeof copy - 1] = '\0';

    const char *delim = " \t";
    char *key = strtok(copy, delim);
    char *val = strtok(NULL,  delim);

    while (key && val) {
        float f = strtof(val, NULL);
        /* TriSonica error frames encode values as about -99.xx */
        if (f <= -90.0f) f = NAN;

        if      (strcmp(key, "S")  == 0) t->S  = f;
        else if (strcmp(key, "S2") == 0) t->S2 = f;
        else if (strcmp(key, "D")  == 0) t->D  = f;
        else if (strcmp(key, "DV") == 0) t->DV = f;
        else if (strcmp(key, "U")  == 0) t->U  = f;
        else if (strcmp(key, "V")  == 0) t->V  = f;
        else if (strcmp(key, "W")  == 0) t->W  = f;
        else if (strcmp(key, "C")  == 0) t->C  = f;
        else if (strcmp(key, "T")  == 0) t->T  = f;
        else if (strcmp(key, "H")  == 0) t->H  = f;
        else if (strcmp(key, "DP") == 0) t->DP = f;
        else if (strcmp(key, "P")  == 0) t->P  = f;
        else if (strcmp(key, "AD") == 0) t->AD = f;
        else if (strcmp(key, "AX") == 0) t->AX = f;
        else if (strcmp(key, "AY") == 0) t->AY = f;
        else if (strcmp(key, "AZ") == 0) t->AZ = f;
        else if (strcmp(key, "PI") == 0) t->PI = f;
        else if (strcmp(key, "RO") == 0) t->RO = f;
        else if (strcmp(key, "MX") == 0) t->MX = f;
        else if (strcmp(key, "MY") == 0) t->MY = f;
        else if (strcmp(key, "MZ") == 0) t->MZ = f;
        else if (strcmp(key, "MD") == 0) t->MD = f;
        else if (strcmp(key, "TD") == 0) t->TD = f;
        /* unknown keys ignored */

        key = strtok(NULL, delim);
        val = strtok(NULL, delim);
    }

    return true;
}

/* ===================== rate-change API (called from widget) ===================== */
/* Public API: your widget calls this. We just record the request; tri thread applies it. */
void tri_request_output_rate(uint8_t mode)
{
    if (mode >= SAMPLE_RATE_COUNT) mode = 0;
    atomic_store_explicit(&s_desired_mode, mode, memory_order_relaxed);
}

/* ===================== the reader thread ===================== */
static void *tri_task(void *arg)
{
    (void)arg;

    /* open R/W so we can send commands on the same handle */
    s_fd = open("/dev/serial0", O_RDWR | O_NOCTTY);
    if (s_fd < 0) {
        USER_LOG_ERROR("TriSonica: cannot open /dev/serial0");
        return NULL;
    }

    /* configure port */
    tri_config_port(s_fd);

    /* initial desired mode comes from your global */
    uint8_t startMode = atomic_load_explicit(&g_sampleRateMode, memory_order_relaxed);
    atomic_store_explicit(&s_desired_mode, startMode, memory_order_relaxed);

    /* apply initial output rate BEFORE switching to non-blocking */
    s_active_mode = 255; /* force apply */
    uint8_t want = atomic_load_explicit(&s_desired_mode, memory_order_relaxed);
    unsigned hz = mode_to_hz(want);
    if (tri_write_cmd(s_fd, "{outputrate %u}", hz) == 0) {
        s_active_mode = want;
        DjiTest_WidgetLogAppend("TriSonica output rate set to %u Hz", hz);
    } else {
        USER_LOG_ERROR("TriSonica: failed to set initial output rate");
    }

    /* now go non-blocking for the line reader */
    tri_set_nonblock(s_fd, 1);

    char buf[256];
    uint64_t tri_seq = 0;

    while (!atomic_load_explicit(&exit_script_and_shutdown, memory_order_relaxed)) {

        /* 1) Apply any pending output-rate change requests */
        uint8_t wantMode = atomic_load_explicit(&s_desired_mode, memory_order_relaxed);
        if (wantMode != s_active_mode) {
            /* temporarily block for clean command send */
            tri_set_nonblock(s_fd, 0);
            unsigned newHz = mode_to_hz(wantMode);
            if (tri_write_cmd(s_fd, "{outputrate %u}", newHz) == 0) {
                s_active_mode = wantMode;
                /* keep your public indicator in sync, if widget didn’t already set it */
                atomic_store_explicit(&g_sampleRateMode, wantMode, memory_order_relaxed);
                DjiTest_WidgetLogAppend("TriSonica output rate set to %u Hz", newHz);
            } else {
                USER_LOG_ERROR("TriSonica: failed to set output rate");
            }
            tri_set_nonblock(s_fd, 1);
        }

        /* 2) Try read a line (non-blocking) */
        int n = read_line(s_fd, buf, sizeof buf);
        if (n <= 0) {
            continue; /* no data yet – loop */
        }

        /* 3) Parse and publish */
        TriData tmp;
        if (!parse_tri(buf, &tmp)) {
            continue;
        }
        tmp.seq = ++tri_seq;

        atomic_store_explicit(&g_tri, tmp, memory_order_relaxed);

        /* 4) Optional: show on widget when logging */
        if (atomic_load_explicit(&logging_active, memory_order_relaxed)) {
            uint8_t mode = atomic_load_explicit(&g_triDisplayMode, memory_order_relaxed);
            uint8_t hzIdx = atomic_load_explicit(&g_sampleRateMode, memory_order_relaxed);
            const char *hzTxt = g_hzlabel[hzIdx];

            switch (mode) {
                case 0:  // Temperature
                    DjiTest_WidgetLogAppend("%s - TriSonica T = %.1f °C", hzTxt, tmp.T);
                    if (simulate_fc) USER_LOG_ERROR("TriSonica T = %.1f °C", tmp.T);
                    break;
                case 1:  // Wind speed
                    DjiTest_WidgetLogAppend("%s - TriSonica S = %.2f m/s", hzTxt, tmp.S);
                    if (simulate_fc) USER_LOG_ERROR("TriSonica S = %.2f m/s", tmp.S);
                    break;
                case 2:  // Humidity
                    DjiTest_WidgetLogAppend("%s - TriSonica H = %.1f %%", hzTxt, tmp.H);
                    if (simulate_fc) USER_LOG_ERROR("TriSonica H = %.1f %%", tmp.H);
                    break;
            }
        }
    }

    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    return NULL;
}

void tri_reader_start(pthread_t *th)
{
    pthread_create(th, NULL, tri_task, NULL);
}
