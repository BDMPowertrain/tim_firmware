/* bms_params.c — the BMS's parameter table, see bms_params.h. A stored ID
   is never reused: a parameter that goes away leaves its ID unused. Names
   are provisional (#28). */

#include "bms_params.h"

static const param_def_t bmsDefs[BMS_PARAM_COUNT] = {
    /*                              id  name                 type        def   min   max */
    [BMS_PARAM_CAN1_TERM]         = { 1u, "can1_term",         PARAM_BOOL,    0,    0,    1 },
    [BMS_PARAM_CAN2_TERM]         = { 2u, "can2_term",         PARAM_BOOL,    0,    0,    1 },
    [BMS_PARAM_CHARGE_TARGET]     = { 3u, "charge_target",     PARAM_U16,  4100, 3600, 4180 },
    [BMS_PARAM_FULL_CHARGE_LIMIT] = { 4u, "full_charge_limit", PARAM_U16,    20,    0,  120 },
};

const param_table_t bms_param_table = {
    .image = PARAMS_IMAGE_BMS,
    .defs = bmsDefs,
    .count = BMS_PARAM_COUNT,
};
