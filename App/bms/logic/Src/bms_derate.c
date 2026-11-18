/* bms_derate.c — the Discharge limit, see bms_derate.h. */

#include "bms_derate.h"

#include <string.h>

void bms_derate_init(bms_derate_t *d, const bms_derate_config_t *config)
{
    memset(d, 0, sizeof *d);
    d->config = *config;
}

/* A linear taper on a value that falls toward trouble: BMS_DERATE_FULL_PPM
   at or above full, BMS_DERATE_FLOOR_PPM at or below floor, a straight line
   between. full > floor (the parameter bounds keep them apart). */
static uint32_t taper(int32_t x, int32_t full, int32_t floor)
{
    if (x >= full) {
        return BMS_DERATE_FULL_PPM;
    }
    if (x <= floor) {
        return BMS_DERATE_FLOOR_PPM;
    }
    const uint64_t range = BMS_DERATE_FULL_PPM - BMS_DERATE_FLOOR_PPM;
    return BMS_DERATE_FLOOR_PPM +
           (uint32_t)((range * (uint32_t)(x - floor)) / (uint32_t)(full - floor));
}

/* The minimum cell voltage with its sag added back: + pack current x cell
   resistance, in mV (0.1 A x uOhm / 10000), at most BMS_DERATE_SAG_MAX_MV.
   Raw with the current invalid. */
static int32_t compensated_mv(const bms_derate_config_t *c, uint16_t mv, bms_current_t i)
{
    if (!i.valid) {
        return mv;
    }
    int32_t sag = ((int32_t)i.deci_a * (int32_t)c->cell_res_uohm) / 10000;
    if (sag > BMS_DERATE_SAG_MAX_MV) {
        sag = BMS_DERATE_SAG_MAX_MV;
    }
    return (int32_t)mv + sag;
}

uint32_t bms_derate_rate_limited(uint32_t fraction_ppm, uint32_t target_ppm, uint32_t gap_ms)
{
    if (gap_ms > BMS_DERATE_MAX_GAP_MS) {
        gap_ms = BMS_DERATE_MAX_GAP_MS;
    }
    if (target_ppm < fraction_ppm) {
        const uint32_t step = (BMS_DERATE_FALL_PPM_PER_S / 1000u) * gap_ms;
        return (fraction_ppm - target_ppm > step) ? fraction_ppm - step : target_ppm;
    }
    const uint32_t step = (BMS_DERATE_RISE_PPM_PER_S / 1000u) * gap_ms;
    return (target_ppm - fraction_ppm > step) ? fraction_ppm + step : target_ppm;
}

uint16_t bms_derate_step(bms_derate_t *d, uint32_t now_ms, bool swept,
                         uint16_t min_cell_mv, int16_t max_temp_dc,
                         bms_current_t current, bool bms_ok)
{
    const bms_derate_config_t *c = &d->config;

    if (swept) {
        const int32_t mv = compensated_mv(c, min_cell_mv, current);
        const uint32_t v = taper(mv, c->v_full_mv, c->v_floor_mv);
        /* Temperature rises toward trouble: taper on its negative. */
        const uint32_t t = taper(-(int32_t)max_temp_dc, -(int32_t)c->t_full_dc,
                                 -(int32_t)c->t_floor_dc);
        const uint32_t target = (v < t) ? v : t;

        d->fraction_ppm = d->swept ? bms_derate_rate_limited(d->fraction_ppm, target,
                                                             now_ms - d->last_ms)
                                   : target; /* the first: at once */
        d->under_low_voltage_limit = mv < (int32_t)c->v_full_mv;
        d->swept = true;
        d->last_ms = now_ms;
    }
    if (!bms_ok) {
        return 0u;
    }
    return (uint16_t)(((uint64_t)c->full_limit_da * d->fraction_ppm) / BMS_DERATE_FULL_PPM);
}

bool bms_derate_under_low_voltage_limit(const bms_derate_t *d)
{
    return d->under_low_voltage_limit;
}
