/*
 * The VCU's parameter table (ADR 0015): the calibrations and
 * configurations a person sets at the car. Read with params_value() and a
 * vcu_param_t index; params.h has the storage rules.
 *
 * Defaults never allow driving: every end point 0 and a calibration
 * revision of 0 (not Calibrated), maximum torque 0, termination open.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_PARAMS_H
#define VCU_PARAMS_H

#include "params.h"

/* The revision of the way this image reads the pedal and brake sensors.
   Bump it when the sensor path changes, so a stored calibration from
   before is not Calibrated. Never 0, which is the default. */
#define VCU_CALIBRATION_REVISION 1

/* The most the maximum-torque parameter may be, in 0.1 N.m: 130 N.m. */
#define VCU_MAX_TORQUE_CEILING 1300

/* Indexes into vcu_param_table. Order only; the stored IDs are in the
   table. */
typedef enum {
    VCU_PARAM_APPS1_MIN,     /* ADC counts at 0% Pedal travel */
    VCU_PARAM_APPS1_MAX,     /* ADC counts at 100% Pedal travel */
    VCU_PARAM_APPS2_MIN,
    VCU_PARAM_APPS2_MAX,
    VCU_PARAM_BRAKE1_ZERO,   /* ADC counts at no pressure */
    VCU_PARAM_BRAKE2_ZERO,
    VCU_PARAM_BRAKE_ENGAGED, /* ADC counts above zero: Brake engaged */
    VCU_PARAM_CAL_REVISION,  /* VCU_CALIBRATION_REVISION when captured */
    VCU_PARAM_CAN1_TERM,     /* 1: terminate the Control bus */
    VCU_PARAM_CAN2_TERM,     /* 1: terminate the DAQ bus */
    VCU_PARAM_MAX_TORQUE,    /* 0.1 N.m at 100% Pedal travel */
    VCU_PARAM_COUNT
} vcu_param_t;

extern const param_table_t vcu_param_table;

#endif /* VCU_PARAMS_H */
