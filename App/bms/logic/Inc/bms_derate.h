/*
 * Derating (ADR 0018): the Discharge limit, lowered by two linear tapers
 * and moved at bounded rates.
 *
 *   voltage taper      100% with the minimum cell voltage at or above
 *                      v_full_mv (the Low-voltage limit), falling linearly
 *                      to 20% at v_floor_mv and held at 20% below it. The
 *                      voltage is sag-compensated: the Sweep's minimum +
 *                      pack current x cell resistance, never more than
 *                      BMS_DERATE_SAG_MAX_MV above it. With pack current
 *                      invalid it is the raw voltage, the conservative side.
 *   temperature taper  100% with the maximum cell temperature at or below
 *                      t_full_dc, falling linearly to 20% at t_floor_dc and
 *                      held at 20% above it.
 * The lower taper wins. The limit follows that target at no more than
 * BMS_DERATE_FALL_PPM_PER_S falling and BMS_DERATE_RISE_PPM_PER_S rising
 * (constants: one bad Sweep costs 4% and not the whole limit). The first
 * Sweep sets it to the target at once.
 *
 * The limit sent is the full discharge limit x that fraction, and 0 A
 * while BMS_OK is low (spec #36: "the BMS sends zero limits until BMS_OK
 * is up"). The fraction keeps following the target meanwhile, so BMS_OK
 * rising gives the limit the cells allow at once.
 *
 * The break points, the cell resistance and the full limit are BMS
 * parameters, each bounded in bms_params.c so no setting puts a taper's
 * end past its fault threshold (ADR 0018). The under-voltage fault stays
 * on the raw loaded voltage (bms_control.h).
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_DERATE_H
#define BMS_DERATE_H

#include <stdbool.h>
#include <stdint.h>

#define BMS_DERATE_FULL_PPM  1000000u /* 100% */
#define BMS_DERATE_FLOOR_PPM 200000u  /* 20%: each taper's end */

/* The bounded rates (ADR 0018), in parts per million of the full limit per
   second: 200% and 10%. */
#define BMS_DERATE_FALL_PPM_PER_S 2000000u
#define BMS_DERATE_RISE_PPM_PER_S 100000u
/* A longer gap between Sweeps moves the limit no further. */
#define BMS_DERATE_MAX_GAP_MS 1000u

/* The most sag put back on a cell, mV: under the 0.375 V at which ADR 0018
   finds the loaded-voltage loop goes unstable, so a wrong resistance or an
   odd current cannot lift a low cell far out of the taper. It also bounds
   the cell resistance parameter (bms_params.h). */
#define BMS_DERATE_SAG_MAX_MV 300

/* Pack current as it enters logic: a value and whether it may be used. The
   source is one replaceable input in bms_step.c (issue 29 decides it). */
typedef struct {
    int16_t deci_a; /* 0.1 A, positive when discharging */
    bool    valid;
} bms_current_t;

/* From the BMS parameters, fixed until the next reset. */
typedef struct {
    uint16_t v_full_mv;     /* voltage taper: 100% at or above */
    uint16_t v_floor_mv;    /* 20% at or below; under v_full_mv */
    int16_t  t_full_dc;     /* temperature taper, 0.1 C: 100% at or below */
    int16_t  t_floor_dc;    /* 20% at or above; over t_full_dc */
    uint16_t cell_res_uohm; /* one cell's resistance, micro-ohm */
    uint16_t full_limit_da; /* the full (100%) Discharge limit, 0.1 A */
} bms_derate_config_t;

/* bms_derate_init() it; touch nothing inside. */
typedef struct {
    bms_derate_config_t config;
    bool     swept;        /* a Sweep has come in */
    uint32_t last_ms;      /* when the last did */
    uint32_t fraction_ppm; /* of the full limit */
    bool     under_low_voltage_limit; /* at the last Sweep */
} bms_derate_t;

void bms_derate_init(bms_derate_t *d, const bms_derate_config_t *config);

/*
 * One step. When swept, min_cell_mv is the lowest cell voltage and
 * max_temp_dc the highest cell temperature this step's Sweep read validly,
 * and the limit moves toward their target; otherwise it holds. Returns the
 * Discharge limit in 0.1 A: 0 unless bms_ok.
 */
uint16_t bms_derate_step(bms_derate_t *d, uint32_t now_ms, bool swept,
                         uint16_t min_cell_mv, int16_t max_temp_dc,
                         bms_current_t current, bool bms_ok);

/* The bounded rates themselves: fraction_ppm moved toward target_ppm by no
   more than they allow over gap_ms (at most BMS_DERATE_MAX_GAP_MS). The
   Charge limit moves by the same (ADR 0019, bms_charge.h). */
uint32_t bms_derate_rate_limited(uint32_t fraction_ppm, uint32_t target_ppm, uint32_t gap_ms);

/* The compensated minimum cell voltage was under the Low-voltage limit
   (v_full_mv) at the last Sweep: the voltage taper is derating. False
   before any Sweep. */
bool bms_derate_under_low_voltage_limit(const bms_derate_t *d);

#endif /* BMS_DERATE_H */
