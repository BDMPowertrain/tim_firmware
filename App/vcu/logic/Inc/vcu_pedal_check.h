/*
 * The Pedal check's sequence (ADR 0016, issue 59): the driver confirms the
 * stored calibration against the real pedals, with the Tractive System
 * off. vcu_control.c decides when it runs and writes its Events; this
 * module only follows the pedals, one step at a time.
 *
 * The accelerator, in order:
 *   RELEASE        both APPS below 5% Pedal travel
 *   FLOOR          both APPS above 95%
 *   RELEASE_AGAIN  both APPS below 5%
 *   DONE
 * and the brake, alongside: Brake engaged reads false with both brake
 * sensors OK, and then true. The check passes on the step both are done.
 *
 * Once the accelerator has left RELEASE the check is under way, and from
 * then until it passes it fails, and starts over from RELEASE, on any step
 * where:
 *   APART   the two APPS are more than 10 points apart
 *   SENSOR  an APPS or a brake sensor reading is not OK
 * Before that nothing fails, so a pedal at rest writes no Event.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_PEDAL_CHECK_H
#define VCU_PEDAL_CHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "vcu_sensors.h"

#define VCU_PEDAL_CHECK_FLOOR  950u /* above 95%, in tenths */
#define VCU_PEDAL_CHECK_AGREE  100u /* 10 points, in tenths */
#define VCU_PEDAL_CHECK_REST   50u  /* below 5%, in tenths */

typedef enum {
    VCU_PEDAL_CHECK_RELEASE,
    VCU_PEDAL_CHECK_FLOOR_IT,
    VCU_PEDAL_CHECK_RELEASE_AGAIN,
    VCU_PEDAL_CHECK_DONE,
} vcu_pedal_check_phase_t;

/* A step's outcome; also byte 0 of an EVENT_PEDAL_CHECK_FAILED Event. */
typedef enum {
    VCU_PEDAL_CHECK_NONE = 0,
    VCU_PEDAL_CHECK_FAIL_APART = 1,
    VCU_PEDAL_CHECK_FAIL_SENSOR = 2,
    VCU_PEDAL_CHECK_PASS = 3,
} vcu_pedal_check_result_t;

typedef struct {
    uint8_t phase;          /* vcu_pedal_check_phase_t */
    bool    brake_released; /* seen released, sensors OK */
    bool    brake_engaged;  /* seen engaged after that */
} vcu_pedal_check_t;

/* Back to the start: nothing seen. */
void vcu_pedal_check_reset(vcu_pedal_check_t *pc);

/* One step of the sequence, from this step's readings and Brake engaged.
   A fail has already reset the sequence; a pass leaves it DONE. */
vcu_pedal_check_result_t vcu_pedal_check_step(vcu_pedal_check_t *pc, const vcu_sensors_t *sn,
                                              bool brake_engaged);

#endif /* VCU_PEDAL_CHECK_H */
