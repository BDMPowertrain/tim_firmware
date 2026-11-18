/* bms_tx.c — what the BMS sends on the Control bus, see bms_tx.h. */

#include "bms_tx.h"

#include <string.h>

#include "cell_temp.h"

bool bms_tx_status_due(uint32_t step_count)
{
    (void)step_count;
    return true;
}

bool bms_tx_power_due(uint32_t step_count)
{
    (void)step_count;
    return true;
}

bms_tx_slow_t bms_tx_slow_due(uint32_t step_count)
{
    return (bms_tx_slow_t)(step_count % (uint32_t)BMS_TX_SLOW_COUNT);
}

#define CHARGER_REQUEST_PERIOD 50u /* steps: 1 s */
#define CHARGER_REQUEST_STEP   1u  /* a BMS_TX_SLOW_INVERTER_LIMITS step */
_Static_assert(CHARGER_REQUEST_STEP % BMS_TX_SLOW_COUNT == BMS_TX_SLOW_INVERTER_LIMITS,
               "the charger request takes a 0x202 slot");
_Static_assert(CHARGER_REQUEST_PERIOD % BMS_TX_SLOW_COUNT == 0u,
               "the charger request keeps to the 0x202 slot each second");

bool bms_tx_charger_request_due(uint32_t step_count)
{
    return step_count % CHARGER_REQUEST_PERIOD == CHARGER_REQUEST_STEP;
}

/* The lowest and highest of a run of readings, and where they are. */
typedef struct {
    uint16_t n;
    int32_t  sum;
    int32_t  min;
    int32_t  max;
    uint8_t  min_at;
    uint8_t  max_at;
} extremes_t;

static void take(extremes_t *e, int32_t x, size_t at)
{
    if (e->n == 0u || x < e->min) {
        e->min = x;
        e->min_at = (uint8_t)at;
    }
    if (e->n == 0u || x > e->max) {
        e->max = x;
        e->max_at = (uint8_t)at;
    }
    e->sum += x;
    e->n++;
}

void bms_tx_summarise(const bq_sweep_t *s, bms_pack_summary_t *out)
{
    extremes_t cells = {0};
    extremes_t temps = {0};

    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        if ((s->devices_read & (1u << (i / BQ_SWEEP_CELLS_PER_DEVICE))) == 0u) {
            continue;
        }
        if (s->cell_mv[i] != 0u) {
            take(&cells, s->cell_mv[i], i);
        }
        int16_t dc;
        if (cell_temp_from_mv(s->temp_mv[i], &dc)) {
            take(&temps, dc, i);
        }
    }
    *out = (bms_pack_summary_t){
        .cells = cells.n,
        .sum_mv = (uint32_t)cells.sum,
        .min_mv = (uint16_t)cells.min,
        .max_mv = (uint16_t)cells.max,
        .min_cell = cells.min_at,
        .max_cell = cells.max_at,
        .temps = temps.n,
        .sum_dc = temps.sum,
        .min_dc = (int16_t)temps.min,
        .max_dc = (int16_t)temps.max,
        .min_temp = temps.min_at,
        .max_temp = temps.max_at,
    };
}

static uint16_t clamp_u16(uint32_t v)
{
    return (v > UINT16_MAX) ? UINT16_MAX : (uint16_t)v;
}

/* Pack voltage in 0.1 V. */
static uint16_t pack_voltage_dv(const bms_pack_summary_t *p)
{
    return clamp_u16(p->sum_mv / 100u);
}

static int16_t avg_dc(const bms_pack_summary_t *p)
{
    return (p->temps == 0u) ? 0 : (int16_t)(p->sum_dc / (int32_t)p->temps);
}

static uint16_t not_below_zero(int16_t dc)
{
    return (dc < 0) ? 0u : (uint16_t)dc;
}

static uint8_t module_of(uint8_t index)
{
    return (uint8_t)(index / BQ_SWEEP_CELLS_PER_DEVICE);
}

static uint8_t number_in_module(uint8_t index)
{
    return (uint8_t)(index % BQ_SWEEP_CELLS_PER_DEVICE);
}

static uint8_t *start(bms_tx_frame_t *f, uint32_t id, uint8_t len)
{
    memset(f, 0, sizeof *f);
    f->id = id;
    f->extended = false;
    f->len = len;
    return f->data;
}

/* The Data ID is the CAN ID (can/control.dbc). */
static void protect(e2e_p11_tx_t *tx, bms_tx_frame_t *f)
{
    e2e_p11_protect(tx, (uint16_t)f->id, f->data, f->len);
}

void bms_tx_build_status(const struct can_control_bms_status_t *signals,
                         bms_tx_e2e_t *e2e, bms_tx_frame_t *f)
{
    uint8_t *d = start(f, CAN_CONTROL_BMS_STATUS_FRAME_ID, CAN_CONTROL_BMS_STATUS_LENGTH);
    (void)can_control_bms_status_pack(d, signals, f->len);
    protect(&e2e->status, f);
}

void bms_tx_build_power(const bms_tx_values_t *v, bms_tx_e2e_t *e2e, bms_tx_frame_t *f)
{
    const struct can_control_bms_power_t p = {
        .pack_current = v->pack_current.valid ? v->pack_current.deci_a : 0,
        .pack_voltage = pack_voltage_dv(v->summary),
        .discharge_limit = v->discharge_limit_da,
    };
    uint8_t *d = start(f, CAN_CONTROL_BMS_POWER_FRAME_ID, CAN_CONTROL_BMS_POWER_LENGTH);
    (void)can_control_bms_power_pack(d, &p, f->len);
    protect(&e2e->power, f);
}

void bms_tx_build_charger_request(const struct can_control_chg_request_t *r,
                                  bms_tx_frame_t *f)
{
    uint8_t *d = start(f, CAN_CONTROL_CHG_REQUEST_FRAME_ID, CAN_CONTROL_CHG_REQUEST_LENGTH);
    f->extended = true;
    (void)can_control_chg_request_pack(d, r, f->len);
}

void bms_tx_build_slow(bms_tx_slow_t slow, const bms_tx_values_t *v, bms_tx_e2e_t *e2e,
                       bms_tx_frame_t *f)
{
    const bms_pack_summary_t *p = v->summary;

    switch (slow) {
    case BMS_TX_SLOW_LIMITS: {
        const struct can_control_bms_limits_t l = {
            .charge_limit = v->charge_limit_da,
            .pack_voltage = pack_voltage_dv(p),
        };
        uint8_t *d = start(f, CAN_CONTROL_BMS_LIMITS_FRAME_ID, CAN_CONTROL_BMS_LIMITS_LENGTH);
        (void)can_control_bms_limits_pack(d, &l, f->len);
        protect(&e2e->limits, f);
        break;
    }
    case BMS_TX_SLOW_INVERTER_LIMITS: {
        const struct can_control_inv_bms_limits_t l = {
            .discharge_limit = (uint16_t)((v->discharge_limit_da + 9u) / 10u),
            .charge_limit = (uint16_t)((v->charge_limit_da + 9u) / 10u),
        };
        uint8_t *d = start(f, CAN_CONTROL_INV_BMS_LIMITS_FRAME_ID,
                           CAN_CONTROL_INV_BMS_LIMITS_LENGTH);
        (void)can_control_inv_bms_limits_pack(d, &l, f->len);
        break;
    }
    case BMS_TX_SLOW_EM_CELL_TEMPS: {
        const struct can_control_em_cell_temps_t t = {
            .max_cell_temp = not_below_zero(p->max_dc),
            .min_cell_temp = not_below_zero(p->min_dc),
            .avg_cell_temp = not_below_zero(avg_dc(p)),
        };
        uint8_t *d = start(f, CAN_CONTROL_EM_CELL_TEMPS_FRAME_ID, CAN_CONTROL_EM_CELL_TEMPS_LENGTH);
        (void)can_control_em_cell_temps_pack(d, &t, f->len);
        break;
    }
    case BMS_TX_SLOW_PACK_VOLTAGES: {
        /* 0.1 mV */
        const struct can_control_bms_pack_voltages_t c = {
            .max_cell_voltage = clamp_u16((uint32_t)p->max_mv * 10u),
            .min_cell_voltage = clamp_u16((uint32_t)p->min_mv * 10u),
            .avg_cell_voltage = (p->cells == 0u) ? 0u
                                                 : clamp_u16((p->sum_mv * 10u) / p->cells),
            .max_cell_module = module_of(p->max_cell),
            .max_cell_number = number_in_module(p->max_cell),
            .min_cell_module = module_of(p->min_cell),
            .min_cell_number = number_in_module(p->min_cell),
        };
        uint8_t *d = start(f, CAN_CONTROL_BMS_PACK_VOLTAGES_FRAME_ID,
                           CAN_CONTROL_BMS_PACK_VOLTAGES_LENGTH);
        (void)can_control_bms_pack_voltages_pack(d, &c, f->len);
        break;
    }
    case BMS_TX_SLOW_PACK_TEMPS: {
        const struct can_control_bms_pack_temps_t t = {
            .max_temp = p->max_dc,
            .min_temp = p->min_dc,
            .avg_temp = avg_dc(p),
            .max_temp_module = module_of(p->max_temp),
            .max_temp_sensor = number_in_module(p->max_temp),
            .min_temp_module = module_of(p->min_temp),
            .min_temp_sensor = number_in_module(p->min_temp),
        };
        uint8_t *d = start(f, CAN_CONTROL_BMS_PACK_TEMPS_FRAME_ID,
                           CAN_CONTROL_BMS_PACK_TEMPS_LENGTH);
        (void)can_control_bms_pack_temps_pack(d, &t, f->len);
        break;
    }
    default:
        break;
    }
}
