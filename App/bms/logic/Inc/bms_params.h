/*
 * The BMS's parameter table (ADR 0015): configurations a person sets at
 * the car, each bounded here. Read with params_value() and a bms_param_t
 * index; params.h has the storage rules.
 *
 * No protection threshold is a parameter. The discharge taper's break
 * points, the cell resistance and the full discharge limit are Derating
 * points (ADR 0018, bms_derate.h), each bounded so no setting puts a
 * taper's end past its fault threshold and every full point stays clear of
 * its floor. The charge taper points join with charge control (#67).
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
    BMS_PARAM_DISCHARGE_V_FULL,  /* mV: the voltage taper's 100% point */
    BMS_PARAM_DISCHARGE_V_FLOOR, /* mV: its 20% point */
    BMS_PARAM_DISCHARGE_T_FULL,  /* 0.1 C: the temperature taper's 100% point */
    BMS_PARAM_DISCHARGE_T_FLOOR, /* 0.1 C: its 20% point */
    BMS_PARAM_CELL_RESISTANCE,   /* micro-ohm, one cell, for sag compensation */
    BMS_PARAM_FULL_DISCHARGE_LIMIT, /* 0.1 A: the Discharge limit at 100% */
    BMS_PARAM_COUNT
} bms_param_t;

/* The cell datasheet's maximum discharge current, 0.1 A: the full
   discharge limit's cap (ADR 0018). PLACEHOLDER: 250 A until the
   datasheet figure is in (issue 93). */
#define BMS_FULL_DISCHARGE_LIMIT_MAX_DA 2500

extern const param_table_t bms_param_table;

#endif /* BMS_PARAMS_H */
