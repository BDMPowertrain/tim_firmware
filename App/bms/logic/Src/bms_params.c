/* bms_params.c — the BMS's parameter table, see bms_params.h. A stored ID
   is never reused: a parameter that goes away leaves its ID unused. Names
   are provisional (#28). */

#include "bms_params.h"

#include "bms_control.h"
#include "bms_derate.h"

/* The Derating bounds (ADR 0018). The voltage taper's floor stays above
   the under-voltage fault, which is judged on the raw voltage
   (bms_control.h). The temperature floor's maximum, 57.5 C, sits under the
   over-temperature fault's 58 C (BMS_OT_MV, checked through the sensor
   table by the host test). Every full point is clear of every floor, so no
   setting makes a step or a taper that runs backwards. */
#define V_FULL_MIN  2800
#define V_FULL_MAX  3600
#define V_FLOOR_MIN 2550
#define V_FLOOR_MAX 2790
#define T_FULL_MIN  300
#define T_FULL_MAX  540
#define T_FLOOR_MIN 545
#define T_FLOOR_MAX 575
_Static_assert(V_FLOOR_MIN > (int)BMS_UV_MV, "the voltage taper ends above the UV fault");
_Static_assert(V_FULL_MIN > V_FLOOR_MAX, "the voltage taper's points never meet");
_Static_assert(T_FLOOR_MIN > T_FULL_MAX, "the temperature taper's points never meet");

/* The charge taper bounds (ADR 0019). The hot zero's maximum, 57.5 C,
   sits under the over-temperature fault's 58 C, and the cold zero's
   minimum, 3.0 C, above the Charge-mode under-temperature fault's 2 C
   (BMS_UT_MV; both checked through the sensor table by the host test).
   The Charge target's 4.18 V maximum keeps the voltage taper's zero under
   the over-voltage fault. */
#define CHARGE_TARGET_MIN 3600
#define CHARGE_TARGET_MAX 4180
#define V_BAND_MIN    20
#define V_BAND_MAX    200
#define T_HOT_FULL_MIN  300
#define T_HOT_FULL_MAX  540
#define T_HOT_ZERO_MIN  545
#define T_HOT_ZERO_MAX  575
#define T_COLD_FULL_MIN 60
#define T_COLD_FULL_MAX 200
#define T_COLD_ZERO_MIN 30
#define T_COLD_ZERO_MAX 55
_Static_assert(CHARGE_TARGET_MAX < (int)BMS_OV_MV, "the charge ends under the OV fault");
_Static_assert(T_HOT_ZERO_MIN > T_HOT_FULL_MAX, "the hot taper's points never meet");
_Static_assert(T_COLD_FULL_MIN > T_COLD_ZERO_MAX, "the cold taper's points never meet");

/* The cell resistance puts back at most BMS_DERATE_SAG_MAX_MV at the
   largest full discharge limit: uOhm = mV x 10000 / 0.1 A. */
#define CELL_RES_MAX_UOHM \
    ((BMS_DERATE_SAG_MAX_MV * 10000) / BMS_FULL_DISCHARGE_LIMIT_MAX_DA)

static const param_def_t bmsDefs[BMS_PARAM_COUNT] = {
    /*                              id  name                 type        def   min   max  calibration */
    [BMS_PARAM_CAN1_TERM]         = { 1u, "can1_term",         PARAM_BOOL,    0,    0,    1, false },
    [BMS_PARAM_CAN2_TERM]         = { 2u, "can2_term",         PARAM_BOOL,    0,    0,    1, false },
    [BMS_PARAM_CHARGE_TARGET]     = { 3u, "charge_target",     PARAM_U16,  4100,
                                      CHARGE_TARGET_MIN, CHARGE_TARGET_MAX, false },
    [BMS_PARAM_FULL_CHARGE_LIMIT] = { 4u, "full_charge_limit", PARAM_U16,    20,    0,  120, false },
    [BMS_PARAM_DISCHARGE_V_FULL]  = { 5u, "discharge_v_full",  PARAM_U16,  3000,
                                      V_FULL_MIN, V_FULL_MAX, false },
    [BMS_PARAM_DISCHARGE_V_FLOOR] = { 6u, "discharge_v_floor", PARAM_U16,  2700,
                                      V_FLOOR_MIN, V_FLOOR_MAX, false },
    [BMS_PARAM_DISCHARGE_T_FULL]  = { 7u, "discharge_t_full",  PARAM_I16,   500,
                                      T_FULL_MIN, T_FULL_MAX, false },
    [BMS_PARAM_DISCHARGE_T_FLOOR] = { 8u, "discharge_t_floor", PARAM_I16,   570,
                                      T_FLOOR_MIN, T_FLOOR_MAX, false },
    [BMS_PARAM_CELL_RESISTANCE]   = { 9u, "cell_resistance",   PARAM_U16,     0,
                                      0, CELL_RES_MAX_UOHM, false },
    [BMS_PARAM_FULL_DISCHARGE_LIMIT] = { 10u, "full_discharge_limit", PARAM_U16, 200,
                                         0, BMS_FULL_DISCHARGE_LIMIT_MAX_DA, false },
    [BMS_PARAM_CHARGE_V_BAND]     = { 11u, "charge_v_band",    PARAM_U16,    50,
                                      V_BAND_MIN, V_BAND_MAX, false },
    [BMS_PARAM_CHARGE_T_HOT_FULL] = { 12u, "charge_t_hot_full", PARAM_I16,  500,
                                      T_HOT_FULL_MIN, T_HOT_FULL_MAX, false },
    [BMS_PARAM_CHARGE_T_HOT_ZERO] = { 13u, "charge_t_hot_zero", PARAM_I16,  550,
                                      T_HOT_ZERO_MIN, T_HOT_ZERO_MAX, false },
    [BMS_PARAM_CHARGE_T_COLD_FULL] = { 14u, "charge_t_cold_full", PARAM_I16, 100,
                                       T_COLD_FULL_MIN, T_COLD_FULL_MAX, false },
    [BMS_PARAM_CHARGE_T_COLD_ZERO] = { 15u, "charge_t_cold_zero", PARAM_I16,  50,
                                       T_COLD_ZERO_MIN, T_COLD_ZERO_MAX, false },
};

const param_table_t bms_param_table = {
    .image = PARAMS_IMAGE_BMS,
    .defs = bmsDefs,
    .count = BMS_PARAM_COUNT,
};
