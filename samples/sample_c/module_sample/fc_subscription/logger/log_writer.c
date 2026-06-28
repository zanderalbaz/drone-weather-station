/* log_writer.c – writes every fused field + full TriSonica frame  -------- */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <pthread.h>
#include <dji_logger.h>
#include "logger.h"

#define QSIZE  4096
static LogRow        ring[QSIZE];
static _Atomic uint64_t widx = 0;
static _Atomic uint64_t ridx = 0;
static _Atomic uint64_t queue_dropped_rows = 0;

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

/* ---------------- CSV header (one single line) ------------------------- */
static const char *CSV_HEADER =
"# schema_version,row_index,mono_us,gps_s,"
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

/* ---------------- CSV writer thread ----------------------------------- */
static void *writer_task(void *arg)
{
    g_csvFile = fopen("/home/rmbl/Desktop/Collected Data/fusion_400hz_temp.csv", "w");
    if (!g_csvFile) return NULL;
    setvbuf(g_csvFile, NULL, _IOFBF, 8192);
    fputs(CSV_HEADER, g_csvFile);

    LogRow row;
    for (;;) {

        if (atomic_load_explicit(&exit_script_and_shutdown,
                                 memory_order_relaxed))
            break;
            
        if (g_csvFile == NULL) {
			g_csvFile = fopen("/home/rmbl/Desktop/Collected Data/fusion_400hz_temp.csv", "w");
			if (!g_csvFile) {usleep(100000); continue; }
			setvbuf(g_csvFile, NULL, _IOFBF, 8192);
			fputs(CSV_HEADER, g_csvFile);		
		}

        if (!atomic_load_explicit(&logging_active, memory_order_relaxed)) {
            logger_queue_discard_all();
        }

        while ((atomic_load_explicit(&logging_active, memory_order_relaxed)) && (logger_queue_pop(&row))) {


                fprintf(g_csvFile,
"%u,%" PRIu64 ",%" PRIu64 ",%u,"
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

row.schema_version, row.row_index, row.mono_us, row.gps_s,
row.q[0], row.q[1], row.q[2], row.q[3],
row.ang_raw[0], row.ang_raw[1], row.ang_raw[2],
row.accel_raw[0], row.accel_raw[1], row.accel_raw[2],
row.vel[0], row.vel[1], row.vel[2],
row.accel_body[0], row.accel_body[1], row.accel_body[2],
row.accel_ground[0], row.accel_ground[1], row.accel_ground[2],
row.alt_fused, row.alt_baro, row.height_rel, row.height_fus,
row.pos_vo[0], row.pos_vo[1], row.pos_vo[2],
row.compass[0], row.compass[1], row.compass[2],
row.avoid[0], row.avoid[1], row.avoid[2],
row.avoid[3], row.avoid[4], row.avoid[5],
row.rtk_connect, row.flight_anomaly,
row.gps_pos[0], row.gps_pos[1], row.gps_pos[2],
row.gps_vel[0], row.gps_vel[1], row.gps_vel[2],
row.rtk_pos[0], row.rtk_pos[1], row.rtk_pos[2],
row.rtk_vel[0], row.rtk_vel[1], row.rtk_vel[2], (double)row.rtk_yaw,
row.tri_valid,
row.tri.S,  row.tri.S2, row.tri.D,  row.tri.DV,
row.tri.U,  row.tri.V,  row.tri.W,  row.tri.C,
row.tri.T,  row.tri.H,  row.tri.DP, row.tri.P,  row.tri.AD,
row.tri.AX, row.tri.AY, row.tri.AZ,
row.tri.PI, row.tri.RO,
row.tri.MX, row.tri.MY, row.tri.MZ,
row.tri.MD, row.tri.TD);
            
			
        }
        logger_queue_report_drops();
		if(logging_active){
					fflush(g_csvFile);
		}
		usleep(10*1000);
    }
	if (g_csvFile) {fflush(g_csvFile); fclose(g_csvFile); }
    return NULL;
}

void log_writer_start(pthread_t *th)
{
    pthread_create(th, NULL, writer_task, NULL);
}
