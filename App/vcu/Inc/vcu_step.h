#ifndef APP_VCU_INC_VCU_STEP_H
#define APP_VCU_INC_VCU_STEP_H

/* The VCU's Control step (ADR 0010): every 10 ms, three phases.
     input   start CAN1 on the first step, update the mailboxes, read the
             newest 10 ms of ADC samples, averaged, and the Shutdown
             Circuit monitor and start button (issue 55);
     logic   vcu_control_step() (vcu_control.h): the states, the Start
             procedure, the frames and the outputs, all logic;
     output  send 0x0C0, write the test point, brake light, Ready to Drive
             sound and fault LED, send VCU_Status, post the Events.
   What the step read and decided about the driver's inputs is published
   with the step's snapshot. */

#include <stdbool.h>
#include <stdint.h>

#include "control_step.h"
#include "vcu_control.h"
#include "vcu_sensors.h"

typedef struct {
    vcu_sensor_raw_t raw;       /* averaged ADC counts */
    vcu_sensors_t    sensors;   /* Pedal travel, brake pressure, status */
    bool samples_ready;         /* every analog input has a sample */
    bool sdc_closed;            /* Shutdown Circuit monitor (PA6) reads low */
    bool start_pressed;         /* start button (PA15) reads low */
    bool brake_engaged;
    uint16_t start_blockers;    /* VCU_START_... bits (vcu_control.h) */
} vcu_inputs_t;

/* Reads the Parameters the step needs and sets up the Control bus for the
   frames the VCU reads. Call from tasks_init() after parameters_init() and
   before the step is created. False if CAN1 refused. */
bool vcu_step_init(void);

/* The step itself, for control_step_task(). */
extern const control_step_def_t vcu_step_def;

/* Copies the inputs the last step read. False before the first step.
   Any task. */
bool vcu_step_read_inputs(vcu_inputs_t *out);

/* The VCU's Parameter save hook (parameters.h): a save is accepted only in
   GLV_ON (ADR 0015, vcu_control_save_refusal()). */
const char *vcu_step_save_refusal(void);

/* The VCU's refusal hook for the bench commands `hang`, `crash` and a
   `term` override (shell.h): refused in READY_TO_DRIVE (ADR 0002
   amendment, vcu_control_bench_refusal()). */
const char *vcu_step_bench_refusal(void);

#endif /* APP_VCU_INC_VCU_STEP_H */
