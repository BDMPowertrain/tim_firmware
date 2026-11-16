/* vcu_params.c — the VCU's parameter table, see vcu_params.h. A stored ID
   is never reused: a parameter that goes away leaves its ID unused. Names
   are provisional (#28). */

#include "vcu_params.h"

#define ADC_MAX 4095

static const param_def_t vcuDefs[VCU_PARAM_COUNT] = {
    /*                          id   name             type        def  min  max */
    [VCU_PARAM_APPS1_MIN]     = {  1u, "apps1_min",     PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_APPS1_MAX]     = {  2u, "apps1_max",     PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_APPS2_MIN]     = {  3u, "apps2_min",     PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_APPS2_MAX]     = {  4u, "apps2_max",     PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_BRAKE1_ZERO]   = {  5u, "brake1_zero",   PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_BRAKE2_ZERO]   = {  6u, "brake2_zero",   PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_BRAKE_ENGAGED] = {  7u, "brake_engaged", PARAM_U16,  0,   0, ADC_MAX },
    [VCU_PARAM_CAL_REVISION]  = {  8u, "cal_revision",  PARAM_U16,  0,   0, 65535 },
    [VCU_PARAM_CAN1_TERM]     = {  9u, "can1_term",     PARAM_BOOL, 0,   0, 1 },
    [VCU_PARAM_CAN2_TERM]     = { 10u, "can2_term",     PARAM_BOOL, 0,   0, 1 },
    [VCU_PARAM_MAX_TORQUE]    = { 11u, "max_torque",    PARAM_U16,  0,   0, VCU_MAX_TORQUE_CEILING },
};

const param_table_t vcu_param_table = {
    .image = PARAMS_IMAGE_VCU,
    .defs = vcuDefs,
    .count = VCU_PARAM_COUNT,
};
