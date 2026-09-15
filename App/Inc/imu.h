#ifndef APP_INC_IMU_H
#define APP_INC_IMU_H

#include <stdbool.h>
#include <stdint.h>

/* One 3-axis sample, in raw signed LSB counts as the ASM330LHHG1 outputs
   them. Convert with IMU_ACCEL_MG_PER_LSB / IMU_GYRO_MDPS_PER_LSB below,
   matching whatever full-scale range imu_hw_init() (imu.c) configured. */
typedef struct {
    int16_t x, y, z;
} imu_sample_t;

/* CTRL1_XL is configured for FS_XL = +/-8 g; CTRL2_G for FS_G =
   +/-2000 dps (Documentation/asm330lhhg1.pdf section 4, Table
   "Sensitivity"). Change both together if imu.c's register config
   changes. */
#define IMU_ACCEL_MG_PER_LSB   0.244f
#define IMU_GYRO_MDPS_PER_LSB  70.0f

/* The task entry point. Does its own hardware bring-up (WHO_AM_I check,
   FIFO/INT1 configuration) the first time it runs, then blocks on a
   task notification from HAL_GPIO_EXTI_Callback (imu.c) and drains the
   FIFO over spi3Mutex each time IMU_INT's watermark interrupt fires. */
void imuTask(void *argument);

/* Copies the most recently seen accelerometer/gyroscope sample into
   *out. Returns false (and leaves *out untouched) if imuTask hasn't
   drained a first FIFO batch yet. Safe to call from any task — internally
   guarded by a small mutex separate from spi3Mutex, since this only
   touches a cached copy in RAM, not the bus itself. */
bool imu_get_latest_accel(imu_sample_t *out);
bool imu_get_latest_gyro(imu_sample_t *out);

/* Diagnostic: reads FIFO_STATUS1/2 directly, live, over spi3Mutex —
   bypasses imuTask's cached samples entirely, so it answers "is the
   FIFO actually filling" independent of whether IMU_INT/EXTI4 ever
   fires. *pending is the FIFO's own DIFF_FIFO count (entries, not
   sample pairs); *watermark_flag is FIFO_WTM_IA, i.e. whether the FIFO
   itself thinks it has reached the configured threshold right now.
   Returns false only if imuTask never got past WHO_AM_I. */
bool imu_fifo_status(uint16_t *pending, bool *watermark_flag);

#endif /* APP_INC_IMU_H */
