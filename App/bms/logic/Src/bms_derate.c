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
   resistance, in mV (0.1 A x uOhm / 10000). Raw with the current invalid. */
static int32_t compensated_mv(const bms_derate_config_t *c, uint16_t mv, bms_current_t i)
{
    if (!i.valid) {
        return mv;
    }
    return (int32_t)mv + ((int32_t)i.deci_a * (int32_t)c->cell_res_uohm) / 10000;
}

static uint32_t target_ppm(const bms_derate_config_t *c, uint16_t min_cell_mv,
                           int16_t max_temp_dc, bms_current_t current)
{
    const uint32_t v = taper(compensated_mv(c, min_cell_mv, current), c->v_full_mv,
                             c->v_floor_mv);
    /* Temperature rises toward trouble: taper on its negative. */
    const uint32_t t = taper(-(int32_t)max_temp_dc, -(int32_t)c->t_full_dc,
                             -(int32_t)c->t_floor_dc);
    return (v < t) ? v : t;
}

uint16_t bms_derate_step(bms_derate_t *d, uint32_t now_ms, bool measured,
                         uint16_t min_cell_mv, int16_t max_temp_dc,
                         bms_current_t current, bool bms_ok)
{
    const bms_derate_config_t *c = &d->config;

    if (measured) {
        const uint32_t target = target_ppm(c, min_cell_mv, max_temp_dc, current);

        if (!d->measured) {
            d->fraction_ppm = target; /* the first: at once */
        } else {
            uint32_t gap = now_ms - d->last_ms;
            if (gap > BMS_DERATE_MAX_GAP_MS) {
                gap = BMS_DERATE_MAX_GAP_MS;
            }
            if (target < d->fraction_ppm) {
                const uint32_t step = (BMS_DERATE_FALL_PPM_PER_S / 1000u) * gap;
                d->fraction_ppm = (d->fraction_ppm - target > step) ? d->fraction_ppm - step
                                                                     : target;
            } else {
                const uint32_t step = (BMS_DERATE_RISE_PPM_PER_S / 1000u) * gap;
                d->fraction_ppm = (target - d->fraction_ppm > step) ? d->fraction_ppm + step
                                                                     : target;
            }
        }
        d->measured = true;
        d->last_ms = now_ms;
    }
    if (!bms_ok) {
        return 0u;
    }
    return (uint16_t)(((uint64_t)c->full_limit_da * d->fraction_ppm) / BMS_DERATE_FULL_PPM);
}

uint32_t bms_derate_fraction_ppm(const bms_derate_t *d)
{
    return d->fraction_ppm;
}
