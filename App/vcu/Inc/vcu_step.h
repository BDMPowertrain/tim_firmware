#ifndef APP_VCU_INC_VCU_STEP_H
#define APP_VCU_INC_VCU_STEP_H

/* The VCU's Control step (ADR 0010): every 10 ms, snapshot the mailboxes
   and the inputs, run the logic, send. The logic is still empty, so the
   VCU stays in STARTUP and sends VCU_Status every second step.

   The inputs (issue 55) are read once per step in the input phase: the
   newest 10 ms of ADC samples, averaged, scaled to Pedal travel and brake
   pressure from the calibration Parameters (vcu_sensors.h), and the
   Shutdown Circuit monitor and start button. They are published with the
   step's snapshot. */

#include <stdbool.h>

#include "control_step.h"
#include "vcu_sensors.h"

typedef struct {
    vcu_sensor_raw_t raw;       /* averaged ADC counts */
    vcu_sensors_t    sensors;   /* Pedal travel, brake pressure, status */
    bool samples_ready;         /* every analog input has a sample */
    bool sdc_closed;            /* Shutdown Circuit monitor (PA6) reads low */
    bool start_pressed;         /* start button (PA15) reads low */
} vcu_inputs_t;

/* Sets up the Control bus for the frames the VCU reads. Call from
   tasks_init() before the step is created. False if CAN1 refused. */
bool vcu_step_init(void);

/* The step itself, for control_step_task(). */
extern const control_step_def_t vcu_step_def;

/* Copies the inputs the last step read. False before the first step.
   Any task. */
bool vcu_step_read_inputs(vcu_inputs_t *out);

/* The VCU's Parameter save hook (parameters.h): a save is accepted only in
   GLV_ON (ADR 0015), so nothing is written while the car can move. */
const char *vcu_step_save_refusal(void);

/* The VCU's refusal hook for the bench commands `hang`, `crash` and a
   `term` override (shell.h): refused in READY_TO_DRIVE (ADR 0002
   amendment), where any of them could cut the drive with no warning to
   the driver. */
const char *vcu_step_bench_refusal(void);

#endif /* APP_VCU_INC_VCU_STEP_H */
