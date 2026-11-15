/* vcu_shell.c — the shell commands only the VCU image has. shell.c is
   common code and knows nothing about the IMU; these reach it through
   shell_register_commands() (ADR 0001). */

#include "vcu_shell.h"

#include <stdbool.h>
#include <stdint.h>

#include "imu.h"
#include "print.h"
#include "shell.h"

/* mg/mdps conversion done in integer math, not float: this project
   links --specs=nano.specs without -u _printf_float (cmake/gcc-arm-none
   -eabi.cmake), so newlib-nano's vsnprintf silently mis-renders %f.
   IMU_GYRO_MDPS_PER_LSB (70) is already a whole number so raw*70 is
   exact; IMU_ACCEL_MG_PER_LSB (0.244) is scaled by 1000 and truncated,
   fine for a glance-at-it shell readout, not for logged data. */
static int cmd_imu(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    imu_sample_t accel;
    imu_sample_t gyro;

    if (imu_get_latest_accel(&accel)) {
        shell_printf("accel  raw x=%6d y=%6d z=%6d  mg x=%6ld y=%6ld z=%6ld\r\n",
                     accel.x, accel.y, accel.z,
                     (long)accel.x * 244 / 1000, (long)accel.y * 244 / 1000, (long)accel.z * 244 / 1000);
    } else {
        shell_printf("accel  no sample yet\r\n");
    }

    if (imu_get_latest_gyro(&gyro)) {
        shell_printf("gyro   raw x=%6d y=%6d z=%6d  mdps x=%6ld y=%6ld z=%6ld\r\n",
                     gyro.x, gyro.y, gyro.z,
                     (long)gyro.x * 70, (long)gyro.y * 70, (long)gyro.z * 70);
    } else {
        shell_printf("gyro   no sample yet\r\n");
    }

    uint16_t pending;
    bool watermarkFlag;
    if (imu_fifo_status(&pending, &watermarkFlag)) {
        shell_printf("fifo   pending=%u watermark_flag=%u\r\n", pending, (unsigned)watermarkFlag);
    }

    return 0;
}

/* Prints the cached accel/gyro sample roughly once per imuTask drain
   cycle (~100 ms, set by IMU_FIFO_WATERMARK in imu.c) until any key is
   pressed. shell_wait_key()'s timeout does double duty as both
   the print-rate timer and the stop-key check, so there's no separate
   polling loop and no busy-waiting — this task is fully blocked between
   prints. The byte that stops the stream is consumed here and never
   reaches shell_dispatch(), so it doesn't echo or get treated as a
   command; the very next line is a fresh `> ` prompt. Raw counts only
   (not mg/mdps) so the stream stays compact enough to actually watch
   scroll by. */
static int cmd_imustream(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    shell_printf("streaming - press any key to stop\r\n");

    for (;;) {
        imu_sample_t accel;
        imu_sample_t gyro;

        if (imu_get_latest_accel(&accel) && imu_get_latest_gyro(&gyro)) {
            shell_printf("accel x=%6d y=%6d z=%6d  gyro x=%6d y=%6d z=%6d\r\n",
                         accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z);
        }

        if (shell_wait_key(100)) {
            break;
        }
    }

    return 0;
}

static const shell_cmd_t vcuCommands[] = {
    { "imu",       cmd_imu,       "latest accel/gyro sample" },
    { "imustream", cmd_imustream, "live accel/gyro stream, any key to stop" },
};

void vcu_shell_register(void)
{
    shell_register_commands(vcuCommands, sizeof(vcuCommands) / sizeof(vcuCommands[0]));
}
