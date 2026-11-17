/*
 * The VCU's pedal and brake sensors as numbers (issue 55): from the
 * averaged ADC counts and the calibration Parameters, each APPS's
 * **Pedal travel** and each brake sensor's pressure, and for each sensor
 * whether its reading can be used.
 *
 * Wiring (issue 20, Tier 1): every sensor runs from 0-5 V, divided by 0.6
 * into the ADC (3.3 V reference). APPS1 rises with the pedal, APPS2 falls.
 * Sensor supply 1 feeds APPS1 and the front brake sensor, supply 2 feeds
 * APPS2 and the rear one; each supply is divided into its own ADC pin.
 *
 * A sensor's status, the first that applies:
 *   NO_SAMPLE       no ADC sample yet (or the ADC never started)
 *   SUPPLY_FAULT    its sensor supply is outside its window, or unmeasured
 *   NOT_CALIBRATED  its end points cannot scale it (APPS: min not below max;
 *                   brake: zero outside the electrical window)
 *   OUT_OF_RANGE    APPS: more than 10% of its span below its min or above
 *                   its max. Brake: outside the electrical window. An open
 *                   wire or a short to ground or supply lands here
 *                   (T.4.2.10, T.4.3.4).
 *   OK
 * A reading that is not OK has travel and pressure 0. What a failed
 * sensor does to torque is the torque path's (issue 57).
 *
 * The full Calibrated rule (span, revision, window) is issue 59's; this
 * module only refuses end points it cannot scale with.
 *
 * Every threshold here is a constant (ADR 0015). The divider, the supply
 * window and the brake window are provisional until the daughterboard
 * front end is designed (issue 20, hardware handoff).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_SENSORS_H
#define VCU_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

/* ADC counts for a voltage at a sensor connector, in mV: x 0.6 divider,
   12-bit ADC on a 3.3 V reference. 5000 mV is 3722 counts. */
#define VCU_SENSOR_MV_TO_COUNTS(mv) ((uint16_t)(((uint32_t)(mv) * 3u * 4095u) / (5u * 3300u)))

/* A sensor supply is good inside 4.75-5.25 V. */
#define VCU_SUPPLY_MIN_COUNTS VCU_SENSOR_MV_TO_COUNTS(4750u)
#define VCU_SUPPLY_MAX_COUNTS VCU_SENSOR_MV_TO_COUNTS(5250u)

/* A brake pressure sensor's output is valid inside 0.25-4.75 V. */
#define VCU_BRAKE_MIN_COUNTS VCU_SENSOR_MV_TO_COUNTS(250u)
#define VCU_BRAKE_MAX_COUNTS VCU_SENSOR_MV_TO_COUNTS(4750u)

/* APPS out of range: beyond an end point by more than this share of the
   calibrated span, in percent. */
#define VCU_APPS_MARGIN_PERCENT 10

/* Pedal travel is in tenths of a percent: 0 to 1000. */
#define VCU_TRAVEL_FULL 1000u

typedef enum {
    VCU_SENSOR_OK,
    VCU_SENSOR_NO_SAMPLE,
    VCU_SENSOR_SUPPLY_FAULT,
    VCU_SENSOR_NOT_CALIBRATED,
    VCU_SENSOR_OUT_OF_RANGE,
} vcu_sensor_status_t;

/* Index 0 is APPS1 / the front brake / supply 1, index 1 is APPS2 / the
   rear brake / supply 2. */
#define VCU_APPS_COUNT  2u
#define VCU_BRAKE_COUNT 2u

/* Averaged ADC counts; ADC_WINDOW_EMPTY (adc_window.h) for no sample. */
typedef struct {
    uint16_t apps[VCU_APPS_COUNT];
    uint16_t brake[VCU_BRAKE_COUNT];
    uint16_t supply[2];
} vcu_sensor_raw_t;

/* The calibration Parameters in force (vcu_params.h), in ADC counts. */
typedef struct {
    int32_t apps_min[VCU_APPS_COUNT]; /* the lower count end point */
    int32_t apps_max[VCU_APPS_COUNT]; /* the higher count end point */
    int32_t brake_zero[VCU_BRAKE_COUNT];
} vcu_sensor_cal_t;

typedef struct {
    uint16_t            travel; /* tenths of a percent, 0 unless OK */
    vcu_sensor_status_t status;
} vcu_apps_reading_t;

typedef struct {
    uint16_t            pressure; /* ADC counts above zero, 0 unless OK */
    vcu_sensor_status_t status;
} vcu_brake_reading_t;

typedef struct {
    vcu_apps_reading_t  apps[VCU_APPS_COUNT];
    vcu_brake_reading_t brake[VCU_BRAKE_COUNT];
    bool                supply_ok[2];
} vcu_sensors_t;

void vcu_sensors_compute(const vcu_sensor_raw_t *raw, const vcu_sensor_cal_t *cal,
                         vcu_sensors_t *out);

/* For the shell. */
const char *vcu_sensor_status_name(vcu_sensor_status_t status);

#endif /* VCU_SENSORS_H */
