#pragma once

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <math.h>

extern _Atomic bool exit_script_and_shutdown;
extern _Atomic bool logging_active;
extern bool simulate_fc;

#define SAMPLE_RATE_COUNT 4
extern _Atomic(uint8_t) g_sampleRateMode; 

extern _Atomic(uint8_t) g_triDisplayMode;
#define TRI_MODE_COUNT 3

extern FILE *g_csvFile;

void tri_reader_start(pthread_t *th);
void fusion_start(pthread_t *th);
void log_writer_start(pthread_t *th);
void tri_request_output_rate(uint8_t);

/* ------------ structures shared by all threads ---------------- */
typedef struct {            /* TriSonica packet already in SI units */
    float S, S2, D, DV, U, V, W, C, T, H, DP, P, AD;
    float AX, AY, AZ, PI, RO, MX, MY, MZ, MD, TD;
    uint64_t tick_us;       /* monotonic time when parsed           */
} TriData;

typedef struct {            /* one fused row at 400 Hz              */
    uint64_t mono_us;       /* 0-based monotonic clock (µs)         */
    uint32_t gps_s;         /* UTC seconds from FC (5 Hz)           */
    /* Fast FC topics ------------------------------------------- */
    float q[4];             /* QUATERNION (400→200 Hz, repeat OK)   */
    float ang_raw[3];       /* ANGULAR_RATE_RAW (400 Hz)            */
    float accel_raw[3];     /* ACCELERATION_RAW (400 Hz)            */
    /* 200 Hz ---------------------------------------------------- */
    float vel[3];
    float accel_body[3], accel_ground[3];
    float alt_fused, alt_baro;
    float height_rel, height_fus;
    float pos_vo[3];
    /* 100 Hz ---------------------------------------------------- */
    float compass[3];
    float avoid[6];
    /* 50 Hz ----------------------------------------------------- */
    uint8_t rtk_connect;
    uint8_t flight_anomaly;
    /* 5 Hz ------------------------------------------------------ */
    double gps_pos[3];
    float  gps_vel[3];
    double rtk_pos[3];
    float  rtk_vel[3];
    uint16_t rtk_yaw;
    /* TriSonica snapshot --------------------------------------- */
    TriData tri;
} LogRow;

/* ------------ small helper to fill every float in a LogRow with NAN -- */
/*   (integer fields stay at 0)                                             */
static inline void row_set_nan(LogRow *r)
{
    float *fptr = (float *)r;
    size_t count = sizeof(LogRow) / sizeof(float);
    for (size_t i = 0; i < count; i++) {
        fptr[i] = NAN;
    }
}


/* ring-buffer helpers implemented in log_writer.c -------------- */
void logger_queue_push(const LogRow *row);
int  logger_queue_pop (LogRow *row);

#endif /*LOGGER_H*/
