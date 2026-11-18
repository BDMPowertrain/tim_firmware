/* bms_params.c — the BMS's parameter table, see bms_params.h. A stored ID
   is never reused: a parameter that goes away leaves its ID unused. Names
   are provisional (#28). */

#include "bms_params.h"

#include "bms_control.h"

/* The voltage taper's floor stays above the under-voltage fault, which is
   judged on the raw voltage (bms_control.h). The temperature floor's
   maximum, 57.5 C, sits under the over-temperature fault's 58 C (BMS_OT_MV,
   checked through the sensor table by the host test). */
#define V_FLOOR_MIN 2550
_Static_assert(V_FLOOR_MIN > (int)BMS_UV_MV, "the voltage taper ends above the UV fault");

static const param_def_t bmsDefs[BMS_PARAM_COUNT] = {
    /*                              id  name                 type        def   min   max  calibration */
    [BMS_PARAM_CAN1_TERM]         = { 1u, "can1_term",         PARAM_BOOL,    0,    0,    1, false },
    [BMS_PARAM_CAN2_TERM]         = { 2u, "can2_term",         PARAM_BOOL,    0,    0,    1, false },
    [BMS_PARAM_CHARGE_TARGET]     = { 3u, "charge_target",     PARAM_U16,  4100, 3600, 4180, false },
    [BMS_PARAM_FULL_CHARGE_LIMIT] = { 4u, "full_charge_limit", PARAM_U16,    20,    0,  120, false },
    [BMS_PARAM_DISCHARGE_V_FULL]  = { 5u, "dis_v_full",        PARAM_U16,  3000, 2800, 3600, false },
    [BMS_PARAM_DISCHARGE_V_FLOOR] = { 6u, "dis_v_floor",       PARAM_U16,  2700, V_FLOOR_MIN, 2790, false },
    [BMS_PARAM_DISCHARGE_T_FULL]  = { 7u, "dis_t_full",        PARAM_I16,   500,  300,  540, false },
    [BMS_PARAM_DISCHARGE_T_FLOOR] = { 8u, "dis_t_floor",       PARAM_I16,   570,  545,  575, false },
    [BMS_PARAM_CELL_RESISTANCE]   = { 9u, "cell_res_uohm",     PARAM_U16,     0,    0, 10000, false },
    [BMS_PARAM_FULL_DISCHARGE_LIMIT] = { 10u, "full_discharge_limit", PARAM_U16, 200, 0,
                                         BMS_FULL_DISCHARGE_LIMIT_MAX_DA, false },
};

const param_table_t bms_param_table = {
    .image = PARAMS_IMAGE_BMS,
    .defs = bmsDefs,
    .count = BMS_PARAM_COUNT,
};
