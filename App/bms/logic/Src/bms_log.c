#include "bms_log.h"

#include <string.h>

_Static_assert(BMS_LOG_SNAPSHOT_SIZE == 580u, "the snapshot's layout in bms_log.h");

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint8_t bit(bool on, unsigned n)
{
    return on ? (uint8_t)(1u << n) : 0u;
}

bool bms_log_snapshot_due(bool swept, uint32_t sweeps)
{
    return swept && (sweeps % BMS_LOG_SNAPSHOT_SWEEPS) == 0u;
}

void bms_log_pack_encode(uint8_t out[BMS_LOG_PACK_SIZE], const data_log_step_t *common,
                         bq_chain_state_t chain, const bms_control_in_t *in,
                         const bms_control_out_t *decided)
{
    const bq_sweep_t *s = in->sweep;
    const bms_pack_summary_t *p = &decided->summary;
    const bool bridgeRead = (s != NULL) && s->bridge_read;

    data_log_step_encode(out, common);
    out[12] = (uint8_t)(bit(in->swept, 0) | bit(in->sdc_closed, 1) | bit(in->nfault_ok, 2) |
                        bit(in->bms_ok_fb, 3) | bit(in->can_running, 4) |
                        bit(in->bus_off_latched, 5) | bit(in->error_passive, 6) |
                        bit(in->rx_overflowed, 7));
    out[13] = (uint8_t)(bit(decided->bms_ok, 0) | bit(decided->under_low_voltage_limit, 1) |
                        bit(decided->open_wire_due, 2) |
                        bit(in->charger != NULL && in->charger->state == MAILBOX_VALID, 3) |
                        bit(in->pack_current.valid, 4) | bit(bridgeRead, 5) |
                        bit(decided->balance_command.op != BQ_BALANCE_NONE, 6));
    put16(&out[14], decided->active);
    put16(&out[16], decided->seen);
    out[18] = (uint8_t)chain;
    out[19] = (uint8_t)decided->charge;
    out[20] = decided->balance_state;
    out[21] = decided->balance_reason;
    put16(&out[22], (s != NULL) ? s->devices_read : 0u);
    out[24] = bridgeRead ? s->bridge_fault : 0u;
    out[25] = (uint8_t)((p->cells > UINT8_MAX) ? UINT8_MAX : p->cells);
    out[26] = (uint8_t)((p->temps > UINT8_MAX) ? UINT8_MAX : p->temps);
    out[27] = 0u;
    put32(&out[28], p->sum_mv);
    put16(&out[32], p->min_mv);
    put16(&out[34], p->max_mv);
    out[36] = p->min_cell;
    out[37] = p->max_cell;
    put16(&out[38], (uint16_t)p->min_dc);
    put16(&out[40], (uint16_t)p->max_dc);
    out[42] = p->min_temp;
    out[43] = p->max_temp;
    put16(&out[44], (uint16_t)in->pack_current.deci_a);
    put16(&out[46], decided->discharge_limit_da);
    put16(&out[48], decided->charge_limit_da);
    put16(&out[50], decided->cell_spread_mv);
}

void bms_log_snapshot_encode(uint8_t out[BMS_LOG_SNAPSHOT_SIZE], const data_log_step_t *common,
                             bq_chain_state_t chain, const bms_control_in_t *in,
                             const bms_control_out_t *decided)
{
    static const bq_sweep_t none; /* a step with no Sweep in hand */
    const bq_sweep_t *s = (in->sweep != NULL) ? in->sweep : &none;

    bms_log_pack_encode(out, common, chain, in, decided);
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        put16(&out[BMS_LOG_CELLS_AT + 2u * i], s->cell_mv[i]);
        put16(&out[BMS_LOG_TEMPS_AT + 2u * i], s->temp_mv[i]);
    }
    memcpy(&out[BMS_LOG_BLEED_AT], decided->bleeding, BQ_SWEEP_DEVICES);
}
