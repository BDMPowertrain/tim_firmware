/*
 * The pedal and brake calibration (ADR 0015, 0016; issue 59): the rule
 * for **Calibrated**, and the guided capture that turns three held
 * positions into a calibration that passes it.
 *
 * Calibrated means, the first that fails being the reason:
 *   MISSING        an end point, the brake_engaged threshold or the
 *                  revision is not from a valid stored copy
 *   REVISION       the stored calibration revision is not this image's
 *                  VCU_CALIBRATION_REVISION (vcu_params.h)
 *   APPS_ORDER     an APPS's minimum is not below its maximum
 *   APPS_SPAN      an APPS's span is below VCU_APPS_MIN_SPAN
 *   APPS_WINDOW    an APPS end point is outside the electrically valid
 *                  window (VCU_APPS_MIN_COUNTS..VCU_APPS_MAX_COUNTS,
 *                  vcu_sensors.h)
 *   BRAKE_WINDOW   a brake zero, or zero plus the threshold, is outside
 *                  the brake window (VCU_BRAKE_MIN_COUNTS..MAX)
 *   BRAKE_THRESHOLD  brake_engaged is below VCU_BRAKE_ENGAGED_MIN
 * The sensors are checked APPS1, APPS2, front brake, rear brake.
 *
 * The guided capture (the shell's `calibrate`) holds three positions:
 *   released  accelerator and brake both released
 *   floored   accelerator fully pressed
 *   braked    accelerator released, brake pressed firmly
 * Each is an average of steady samples; it is refused (the first that
 * applies) with NO_SAMPLE if any sample is missing, NOT_STEADY if any
 * sensor moved more than VCU_CAL_STEADY_COUNTS while it was held, and
 * APPS_DIRECTION unless both APPS moved, each the way it runs (APPS1 up,
 * APPS2 down), from released to floored: the two sensors must move
 * together. The end points are then the released and floored averages,
 * each brake zero its released average, and brake_engaged
 * VCU_BRAKE_ENGAGED_PERCENT of the smaller brake rise from released to
 * braked. The result must then pass the Calibrated rule itself, so the
 * capture never yields a set that would not be Calibrated.
 *
 * Every threshold is a constant (ADR 0015), provisional until the
 * daughterboard front end is designed (issue 20).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_CALIBRATION_H
#define VCU_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

#include "vcu_sensors.h"

/* An APPS must move at least 1 V from released to floored: 744 counts,
   so a tenth of a percent of Pedal travel is under a count. */
#define VCU_APPS_MIN_SPAN VCU_SENSOR_MV_TO_COUNTS(1000u)

/* brake_engaged must be at least this many counts above zero, about
   100 mV: well clear of the 25-count hysteresis and the noise. */
#define VCU_BRAKE_ENGAGED_MIN 75

/* brake_engaged is this share of the smaller brake rise in the capture. */
#define VCU_BRAKE_ENGAGED_PERCENT 25

/* A sensor held still moves no more than this, in counts (about 30 mV). */
#define VCU_CAL_STEADY_COUNTS 40u

typedef enum {
    VCU_CAL_OK = 0,
    VCU_CAL_MISSING,
    VCU_CAL_REVISION,
    VCU_CAL_APPS_ORDER,
    VCU_CAL_APPS_SPAN,
    VCU_CAL_APPS_WINDOW,
    VCU_CAL_BRAKE_WINDOW,
    VCU_CAL_BRAKE_THRESHOLD,
    /* The capture's own refusals. */
    VCU_CAL_NO_SAMPLE,
    VCU_CAL_NOT_STEADY,
    VCU_CAL_APPS_DIRECTION,
} vcu_cal_reason_t;

/* Which sensor a reason is about, where it is about one. */
typedef enum {
    VCU_CAL_SENSOR_NONE = 0,
    VCU_CAL_SENSOR_APPS1,
    VCU_CAL_SENSOR_APPS2,
    VCU_CAL_SENSOR_BRAKE_FRONT,
    VCU_CAL_SENSOR_BRAKE_REAR,
} vcu_cal_sensor_t;

typedef struct {
    vcu_cal_reason_t reason;
    vcu_cal_sensor_t sensor;
} vcu_cal_result_t;

/* The calibration Parameters as stored (vcu_params.h), in ADC counts. */
typedef struct {
    int32_t apps_min[VCU_APPS_COUNT];
    int32_t apps_max[VCU_APPS_COUNT];
    int32_t brake_zero[VCU_BRAKE_COUNT];
    int32_t brake_engaged;
    int32_t revision;
} vcu_cal_values_t;

/* The Calibrated rule. present: every one of those values came from a
   valid stored copy. */
vcu_cal_result_t vcu_cal_check(const vcu_cal_values_t *v, bool present);

/* One held position: every sample's sum, lowest and highest, by sensor
   (APPS1, APPS2, front brake, rear brake). */
#define VCU_CAL_CHANNELS 4u

typedef struct {
    uint32_t count;
    bool     empty; /* a sample was missing */
    uint32_t sum[VCU_CAL_CHANNELS];
    uint16_t low[VCU_CAL_CHANNELS];
    uint16_t high[VCU_CAL_CHANNELS];
} vcu_cal_capture_t;

void vcu_cal_capture_init(vcu_cal_capture_t *c);

/* Adds one step's averaged counts (vcu_step's published raw values). */
void vcu_cal_capture_add(vcu_cal_capture_t *c, const vcu_sensor_raw_t *raw);

/* The calibration the three positions give, with this image's revision,
   and whether it may be saved: VCU_CAL_OK only if it is Calibrated. */
vcu_cal_result_t vcu_cal_from_captures(const vcu_cal_capture_t *released,
                                       const vcu_cal_capture_t *floored,
                                       const vcu_cal_capture_t *braked,
                                       vcu_cal_values_t *out);

/* For the shell: "not calibrated" reasons, and the sensor's name. */
const char *vcu_cal_reason_name(vcu_cal_reason_t reason);
const char *vcu_cal_sensor_name(vcu_cal_sensor_t sensor);

#endif /* VCU_CALIBRATION_H */
