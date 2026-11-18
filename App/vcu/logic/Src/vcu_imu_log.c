/* vcu_imu_log.c — the IMU Records' pairing, see vcu_imu_log.h. */

#include "vcu_imu_log.h"

#include <string.h>

void vcu_imu_pairer_init(vcu_imu_pairer_t *p)
{
    memset(p, 0, sizeof(*p));
}

size_t vcu_imu_pair(vcu_imu_pairer_t *p, const vcu_imu_entry_t *entries, size_t count,
                    size_t left, uint32_t now_ms, vcu_imu_record_t records[VCU_IMU_PAIRS_MAX])
{
    size_t made = 0u;

    if (count > VCU_IMU_ENTRIES_MAX) {
        count = VCU_IMU_ENTRIES_MAX;
    }
    for (size_t i = 0; i < count; i++) {
        const vcu_imu_entry_t *e = &entries[i];

        if (e->sensor == VCU_IMU_ACCEL) {
            memcpy(p->pending.accel, e->xyz, sizeof e->xyz);
            p->have_accel = true;
        } else if (e->sensor == VCU_IMU_GYRO) {
            memcpy(p->pending.gyro, e->xyz, sizeof e->xyz);
            p->have_gyro = true;
        } else {
            continue;
        }
        if (p->have_accel && p->have_gyro) {
            records[made].sample = p->pending;
            made++;
            p->have_accel = false;
            p->have_gyro = false;
        }
    }

    /* Periods back from the read: the pairs still in the FIFO, then the
       pairs after this one. */
    const uint32_t newer = (uint32_t)(left / 2u);

    for (size_t k = 0; k < made; k++) {
        const uint32_t back = newer + (uint32_t)(made - 1u - k);
        records[k].uptime_ms = now_ms - (back * VCU_IMU_PERIOD_US + 500u) / 1000u;
    }
    return made;
}
