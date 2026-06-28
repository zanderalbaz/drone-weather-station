#pragma once

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stddef.h>
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
    uint64_t seq;           /* internal freshness counter, not CSV  */
} TriData;

typedef struct {            /* one fused row at 400 Hz              */
    uint32_t schema_version;
    uint64_t row_index;
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
    uint8_t tri_valid;
    /* TriSonica snapshot --------------------------------------- */
    TriData tri;
} LogRow;

static inline void tri_set_measurements_nan(TriData *t)
{
    t->S = NAN;  t->S2 = NAN; t->D = NAN;  t->DV = NAN;
    t->U = NAN;  t->V = NAN;  t->W = NAN;  t->C = NAN;
    t->T = NAN;  t->H = NAN;  t->DP = NAN; t->P = NAN;
    t->AD = NAN; t->AX = NAN; t->AY = NAN; t->AZ = NAN;
    t->PI = NAN; t->RO = NAN; t->MX = NAN; t->MY = NAN;
    t->MZ = NAN; t->MD = NAN; t->TD = NAN;
}

static inline void row_set_float_fields_nan(LogRow *r)
{
    for (size_t i = 0; i < 4; i++) r->q[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->ang_raw[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->accel_raw[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->vel[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->accel_body[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->accel_ground[i] = NAN;
    r->alt_fused = NAN;
    r->alt_baro = NAN;
    r->height_rel = NAN;
    r->height_fus = NAN;
    for (size_t i = 0; i < 3; i++) r->pos_vo[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->compass[i] = NAN;
    for (size_t i = 0; i < 6; i++) r->avoid[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->gps_pos[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->gps_vel[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->rtk_pos[i] = NAN;
    for (size_t i = 0; i < 3; i++) r->rtk_vel[i] = NAN;
    tri_set_measurements_nan(&r->tri);
}

/* ring-buffer helpers implemented in log_writer.c -------------- */
void logger_queue_push(const LogRow *row);
int  logger_queue_pop (LogRow *row);

#endif /*LOGGER_H*/
