/*
 * The IMU Records' pairing (issue 61, ADR 0021): from the entries of one
 * ASM330LHHG1 FIFO read, the accelerometer and gyroscope samples paired
 * into DATA_LOG_IMU Records, each with the time it was sampled.
 *
 * Both sensors are batched into the FIFO at 104 Hz (imu.c) and read about
 * every 100 ms, interleaved in the order sampled. Each sample is kept until
 * one of the other sensor joins it, and the two make a Record; a second
 * sample of the same sensor before that replaces the first. A sample left
 * at the end of a read waits for the next read.
 *
 * The FIFO holds no time. The newest Record of a read is dated at the read
 * (now_ms), less one period for each pair of entries the read left in the
 * FIFO (a read takes at most VCU_IMU_ENTRIES_MAX), and each earlier one
 * VCU_IMU_PERIOD_US before the next, rounded to the millisecond: the error
 * is the read's delay after the newest sample in the FIFO, within one
 * period.
 *
 * No Record is dated before the last Record of the read before: a backlog
 * that would date further back than that (the clock stopped while the
 * sensor ran, as in a debugger halt, or the FIFO overflowed and dropped
 * its oldest) is held at that time, so the dates never run backwards.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_IMU_LOG_H
#define VCU_IMU_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data_log_record.h"

/* The FIFO's batch rate, 104 Hz (imu.c, FIFO_CTRL3). */
#define VCU_IMU_PERIOD_US 9615u

/* Entries one read takes at most (imu.c's IMU_FIFO_BURST_MAX_WORDS). */
#define VCU_IMU_ENTRIES_MAX 32u
/* Records one read makes at most: one more than its entries make alone,
   for a sample left by the read before. */
#define VCU_IMU_PAIRS_MAX (VCU_IMU_ENTRIES_MAX / 2u + 1u)

/* An entry's sensor: TAG_SENSOR (FIFO_DATA_OUT_TAG bits 7:3). Any other
   value is ignored. */
#define VCU_IMU_GYRO  0x01u
#define VCU_IMU_ACCEL 0x02u

typedef struct {
    uint8_t sensor;
    int16_t xyz[3];
} vcu_imu_entry_t;

typedef struct {
    data_log_imu_t sample;
    uint32_t       uptime_ms;
} vcu_imu_record_t;

typedef struct {
    data_log_imu_t pending;
    bool           have_accel;
    bool           have_gyro;
    bool           dated;   /* a Record has been dated */
    uint32_t       last_ms; /* the last Record's date */
} vcu_imu_pairer_t;

void vcu_imu_pairer_init(vcu_imu_pairer_t *p);

/* Pairs count entries (at most VCU_IMU_ENTRIES_MAX; more are ignored) read
   at now_ms, with left newer entries still in the FIFO, into records,
   oldest first. Returns the Records made. */
size_t vcu_imu_pair(vcu_imu_pairer_t *p, const vcu_imu_entry_t *entries, size_t count,
                    size_t left, uint32_t now_ms, vcu_imu_record_t records[VCU_IMU_PAIRS_MAX]);

#endif /* VCU_IMU_LOG_H */
