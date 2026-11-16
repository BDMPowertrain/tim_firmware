/*
 * The BMS's parameter table (ADR 0015): configurations a person sets at
 * the car, each bounded here. Read with params_value() and a bms_param_t
 * index; params.h has the storage rules.
 *
 * No protection threshold is a parameter. The discharge taper break
 * points, cell resistance, full discharge limit and charge taper points
 * join this table with Derating and charge control, when their bounds are
 * decided.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_PARAMS_H
#define BMS_PARAMS_H

#include "params.h"

/* Indexes into bms_param_table. Order only; the stored IDs are in the
   table. */
typedef enum {
    BMS_PARAM_CAN1_TERM,         /* 1: terminate the Control bus */
    BMS_PARAM_CAN2_TERM,         /* 1: terminate the DAQ bus */
    BMS_PARAM_CHARGE_TARGET,     /* mV on the highest cell */
    BMS_PARAM_FULL_CHARGE_LIMIT, /* 0.1 A */
    BMS_PARAM_COUNT
} bms_param_t;

extern const param_table_t bms_param_table;

#endif /* BMS_PARAMS_H */
