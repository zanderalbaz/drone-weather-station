/* log_writer.c – writes every fused field + full TriSonica frame  -------- */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <dji_logger.h>
#include <dji_platform.h>
#include "logger.h"

#define QSIZE  4096
#define SAVE_PATH_MAX 512
#define TEMP_CSV_PATH "/home/rmbl/Desktop/Collected Data/fusion_400hz_temp.csv"

static LogRow        ring[QSIZE];
static _Atomic uint64_t widx = 0;
static _Atomic uint64_t ridx = 0;
static _Atomic uint64_t queue_dropped_rows = 0;
static _Atomic bool save_requested = false;
static _Atomic bool csv_session_unsaved = false;
static pthread_mutex_t save_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static char save_requested_path[SAVE_PATH_MAX] = {0};
static const char *CSV_HEADER;

typedef struct {
    uint64_t unix_s;
    uint32_t unix_ns;
    uint64_t mono_us;
} CsvSessionAnchor;

/* -------- tiny single-producer / single-consumer ring-buffer helpers ---- */
void logger_queue_push(const LogRow *row)
{
    if (!atomic_load_explicit(&logging_active, memory_order_relaxed))
        return;

    uint64_t w = atomic_load_explicit(&widx, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ridx, memory_order_acquire);

    if ((w - r) >= QSIZE) {
        atomic_fetch_add_explicit(&queue_dropped_rows, 1, memory_order_relaxed);
        return;
    }

    ring[w % QSIZE] = *row;
    atomic_store_explicit(&widx, w + 1, memory_order_release);
}

int logger_queue_pop(LogRow *out)
{
    uint64_t r = atomic_load_explicit(&ridx, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&widx, memory_order_acquire);

    if (r == w)
        return 0;               /* empty */

    *out = ring[r % QSIZE];
    atomic_store_explicit(&ridx, r + 1, memory_order_release);
    return 1;
}

static void logger_queue_discard_all(void)
{
    uint64_t w = atomic_load_explicit(&widx, memory_order_acquire);
    atomic_store_explicit(&ridx, w, memory_order_release);
}

static void logger_queue_report_drops(void)
{
    static uint64_t last_reported_drops = 0;
    uint64_t dropped = atomic_load_explicit(&queue_dropped_rows, memory_order_relaxed);

    if (dropped >= last_reported_drops + QSIZE) {
        USER_LOG_WARN("CSV logger queue dropped %" PRIu64 " rows total", dropped);
        last_reported_drops = dropped;
    }
}

void logger_request_save_csv(const char *requested_path)
{
    if (atomic_load_explicit(&logging_active, memory_order_relaxed)) {
        USER_LOG_WARN("Stop logging before saving CSV");
        return;
    }

    if (requested_path == NULL || requested_path[0] == '\0') {
        USER_LOG_WARN("CSV save requested with empty filename");
        return;
    }

    pthread_mutex_lock(&save_request_mutex);
    strncpy(save_requested_path, requested_path, sizeof(save_requested_path) - 1);
    save_requested_path[sizeof(save_requested_path) - 1] = '\0';
    atomic_store_explicit(&save_requested, true, memory_order_release);
    pthread_mutex_unlock(&save_request_mutex);

    USER_LOG_INFO("CSV save requested: %s", save_requested_path);
}

bool logger_has_unsaved_session(void)
{
    return atomic_load_explicit(&csv_session_unsaved, memory_order_acquire);
}

static void logger_capture_session_anchor(CsvSessionAnchor *anchor)
{
    struct timespec ts = {0};
    uint32_t ms = 0;
    T_DjiOsalHandler *os = DjiPlatform_GetOsalHandler();

    memset(anchor, 0, sizeof(*anchor));

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        anchor->unix_s = (uint64_t)ts.tv_sec;
        anchor->unix_ns = (uint32_t)ts.tv_nsec;
    } else {
        USER_LOG_WARN("clock_gettime(CLOCK_REALTIME) failed: %s", strerror(errno));
    }

    if (os && os->GetTimeMs &&
        os->GetTimeMs(&ms) == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        anchor->mono_us = (uint64_t)ms * 1000ULL;
    } else {
        USER_LOG_WARN("GetTimeMs failed while capturing CSV session anchor");
    }
}

static void logger_apply_session_anchor(LogRow *row, const CsvSessionAnchor *anchor)
{
    row->session_start_unix_s = anchor->unix_s;
    row->session_start_unix_ns = anchor->unix_ns;
    row->session_start_mono_us = anchor->mono_us;
}

static bool logger_take_save_request(char *out, size_t out_size)
{
    bool requested = false;

    if (!atomic_load_explicit(&save_requested, memory_order_acquire))
        return false;

    pthread_mutex_lock(&save_request_mutex);
    requested = atomic_load_explicit(&save_requested, memory_order_relaxed);
    if (requested) {
        strncpy(out, save_requested_path, out_size - 1);
        out[out_size - 1] = '\0';
        atomic_store_explicit(&save_requested, false, memory_order_release);
    }
    pthread_mutex_unlock(&save_request_mutex);

    return requested;
}

static bool logger_open_session_file(void)
{
    g_csvFile = fopen(TEMP_CSV_PATH, "w");
    if (!g_csvFile) {
        USER_LOG_ERROR("Open CSV temp file failed: %s", strerror(errno));
        return false;
    }

    setvbuf(g_csvFile, NULL, _IOFBF, 8192);
    fputs(CSV_HEADER, g_csvFile);
    return true;
}

static void logger_close_session_file(void)
{
    if (g_csvFile) {
        fflush(g_csvFile);
        fclose(g_csvFile);
        g_csvFile = NULL;
    }
}

static void logger_build_candidate_path(const char *requested_path, unsigned suffix,
                                        char *out, size_t out_size)
{
    const char *slash = strrchr(requested_path, '/');
    const char *dot = strrchr(requested_path, '.');

    if (suffix == 0) {
        snprintf(out, out_size, "%s", requested_path);
        return;
    }

    if (dot && (!slash || dot > slash)) {
        size_t base_len = (size_t)(dot - requested_path);
        snprintf(out, out_size, "%.*s_%03u%s", (int)base_len, requested_path, suffix, dot);
    } else {
        snprintf(out, out_size, "%s_%03u", requested_path, suffix);
    }
}

static bool logger_publish_session_file(const char *requested_path)
{
    char candidate[SAVE_PATH_MAX];

    for (unsigned suffix = 0; suffix < 1000; suffix++) {
        logger_build_candidate_path(requested_path, suffix, candidate, sizeof(candidate));

        if (link(TEMP_CSV_PATH, candidate) == 0) {
            if (unlink(TEMP_CSV_PATH) != 0) {
                USER_LOG_WARN("CSV temp unlink failed after save: %s", strerror(errno));
            }
            USER_LOG_INFO("CSV log saved as %s", candidate);
            return true;
        }

        if (errno == EEXIST)
            continue;

        USER_LOG_ERROR("CSV save failed for %s: %s", candidate, strerror(errno));
        return false;
    }

    USER_LOG_ERROR("CSV save failed: no unique filename available for %s", requested_path);
    return false;
}

static void logger_handle_save_request(bool *closed_session_pending,
                                       uint64_t *rows_written_this_session)
{
    char requested_path[SAVE_PATH_MAX];

    if (!logger_take_save_request(requested_path, sizeof(requested_path)))
        return;

    if (!*closed_session_pending) {
        USER_LOG_WARN("CSV save requested but there is no unsaved stopped session");
        return;
    }

    if (*rows_written_this_session == 0) {
        if (unlink(TEMP_CSV_PATH) != 0 && errno != ENOENT) {
            USER_LOG_WARN("CSV temp unlink failed for empty session: %s", strerror(errno));
        }
        USER_LOG_WARN("CSV session had no rows; not saved");
        *closed_session_pending = false;
        atomic_store_explicit(&csv_session_unsaved, false, memory_order_release);
        return;
    }

    if (logger_publish_session_file(requested_path)) {
        *closed_session_pending = false;
        atomic_store_explicit(&csv_session_unsaved, false, memory_order_release);
    }
}

/* ---------------- CSV header (one single line) ------------------------- */
static const char *CSV_HEADER =
"# schema_version,row_index,mono_us,"
"session_start_unix_s,session_start_unix_ns,session_start_mono_us,"
"gps_s,gps_date_raw,gps_time_raw,gps_time_fc_ms,gps_time_fc_us,"
"q0,q1,q2,q3,"
"ang_x,ang_y,ang_z,"
"acc_x,acc_y,acc_z,"
"vel_x,vel_y,vel_z,"
"acc_body_x,acc_body_y,acc_body_z,"
"acc_gnd_x,acc_gnd_y,acc_gnd_z,"
"alt_fused,alt_baro,height_rel,height_fus,"
"pos_vo_x,pos_vo_y,pos_vo_z,"
"compass_x,compass_y,compass_z,"
"avoid_fl,avoid_fr,avoid_bl,avoid_br,avoid_l,avoid_r,"
"rtk_connect,flight_anomaly,"
"gps_lat,gps_lon,gps_alt,"
"gps_vx,gps_vy,gps_vz,"
"rtk_lat,rtk_lon,rtk_alt,"
"rtk_vx,rtk_vy,rtk_vz,rtk_yaw,"
"tri_valid,"
"tri_S,tri_S2,tri_D,tri_DV,"
"tri_U,tri_V,tri_W,tri_C,tri_T,tri_H,tri_DP,tri_P,tri_AD,"
"tri_AX,tri_AY,tri_AZ,tri_PI,tri_RO,"
"tri_MX,tri_MY,tri_MZ,tri_MD,tri_TD\n";

static void logger_write_csv_row(const LogRow *row)
{
    fprintf(g_csvFile,
"%u,%" PRIu64 ",%" PRIu64 ","
"%" PRIu64 ",%u,%" PRIu64 ","
"%u,%u,%u,%u,%u,"
"%.7g,%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,"
"%u,%u,"
"%.10f,%.10f,%.2f,"
"%.7g,%.7g,%.7g,"
"%.10f,%.10f,%.2f,"
"%.7g,%.7g,%.7g,%.2f,"
"%u,"
"%.7g,%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,%.7g,%.7g,"
"%.7g,%.7g,%.7g,%.7g,%.7g\n",

row->schema_version, row->row_index, row->mono_us,
row->session_start_unix_s, row->session_start_unix_ns, row->session_start_mono_us,
row->gps_s, row->gps_date_raw, row->gps_time_raw, row->gps_time_fc_ms, row->gps_time_fc_us,
row->q[0], row->q[1], row->q[2], row->q[3],
row->ang_raw[0], row->ang_raw[1], row->ang_raw[2],
row->accel_raw[0], row->accel_raw[1], row->accel_raw[2],
row->vel[0], row->vel[1], row->vel[2],
row->accel_body[0], row->accel_body[1], row->accel_body[2],
row->accel_ground[0], row->accel_ground[1], row->accel_ground[2],
row->alt_fused, row->alt_baro, row->height_rel, row->height_fus,
row->pos_vo[0], row->pos_vo[1], row->pos_vo[2],
row->compass[0], row->compass[1], row->compass[2],
row->avoid[0], row->avoid[1], row->avoid[2],
row->avoid[3], row->avoid[4], row->avoid[5],
row->rtk_connect, row->flight_anomaly,
row->gps_pos[0], row->gps_pos[1], row->gps_pos[2],
row->gps_vel[0], row->gps_vel[1], row->gps_vel[2],
row->rtk_pos[0], row->rtk_pos[1], row->rtk_pos[2],
row->rtk_vel[0], row->rtk_vel[1], row->rtk_vel[2], (double)row->rtk_yaw,
row->tri_valid,
row->tri.S,  row->tri.S2, row->tri.D,  row->tri.DV,
row->tri.U,  row->tri.V,  row->tri.W,  row->tri.C,
row->tri.T,  row->tri.H,  row->tri.DP, row->tri.P,  row->tri.AD,
row->tri.AX, row->tri.AY, row->tri.AZ,
row->tri.PI, row->tri.RO,
row->tri.MX, row->tri.MY, row->tri.MZ,
row->tri.MD, row->tri.TD);
}

/* ---------------- CSV writer thread ----------------------------------- */
static void *writer_task(void *arg)
{
    bool was_logging = false;
    bool session_open = false;
    bool closed_session_pending = false;
    uint64_t rows_written_this_session = 0;
    CsvSessionAnchor session_anchor = {0};
    LogRow row;

    for (;;) {

        if (atomic_load_explicit(&exit_script_and_shutdown,
                                 memory_order_relaxed))
            break;

        bool is_logging = atomic_load_explicit(&logging_active, memory_order_relaxed);

        if (is_logging && !was_logging) {
            if (closed_session_pending && rows_written_this_session > 0) {
                USER_LOG_WARN("Previous CSV session is unsaved; refusing to start a new session");
                atomic_store_explicit(&logging_active, false, memory_order_relaxed);
                is_logging = false;
                logger_queue_discard_all();
            } else {
                if (closed_session_pending && rows_written_this_session == 0) {
                    (void)unlink(TEMP_CSV_PATH);
                    closed_session_pending = false;
                    atomic_store_explicit(&csv_session_unsaved, false, memory_order_release);
                }
                logger_queue_discard_all();
                rows_written_this_session = 0;
                logger_capture_session_anchor(&session_anchor);
                session_open = logger_open_session_file();
                if (!session_open) {
                    atomic_store_explicit(&logging_active, false, memory_order_relaxed);
                    is_logging = false;
                } else {
                    closed_session_pending = false;
                    USER_LOG_INFO("CSV logging session opened");
                }
            }
        }

        if (!is_logging && was_logging) {
            while (session_open && logger_queue_pop(&row)) {
                logger_apply_session_anchor(&row, &session_anchor);
                logger_write_csv_row(&row);
                rows_written_this_session++;
            }
            if (session_open) {
                logger_close_session_file();
                session_open = false;
                closed_session_pending = true;
                atomic_store_explicit(&csv_session_unsaved,
                                      rows_written_this_session > 0,
                                      memory_order_release);
                USER_LOG_INFO("CSV logging session closed with %" PRIu64 " rows", rows_written_this_session);
            }
        }

        if (!is_logging) {
            logger_queue_discard_all();
            logger_handle_save_request(&closed_session_pending, &rows_written_this_session);
        }

        while (is_logging && session_open && logger_queue_pop(&row)) {
            logger_apply_session_anchor(&row, &session_anchor);
            logger_write_csv_row(&row);
            rows_written_this_session++;
        }
        logger_queue_report_drops();
        if (is_logging && session_open) {
            fflush(g_csvFile);
        }

        was_logging = is_logging;
        usleep(10*1000);
    }

    if (session_open) {
        logger_close_session_file();
    }
    return NULL;
}

void log_writer_start(pthread_t *th)
{
    pthread_create(th, NULL, writer_task, NULL);
}
