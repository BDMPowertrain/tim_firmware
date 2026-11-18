/* bms_charge.c — charge control, see bms_charge.h. */

#include "bms_charge.h"

#include <string.h>

#include "bms_derate.h"
#include "control_rx.h"
#include "event_codes.h"

#define FULL_PPM 1000000u

void bms_charge_init(bms_charge_t *c, const bms_charge_config_t *config)
{
    memset(c, 0, sizeof *c);
    c->config = *config;
    c->state = BMS_CHARGE_OFF;
}

static void add_event(bms_charge_events_t *events, uint16_t code, const uint8_t *data)
{
    if (events->count < BMS_CHARGE_EVENTS_MAX) {
        bms_charge_event_t *e = &events->events[events->count++];
        e->code = code;
        memcpy(e->data, data, EVENT_DATA_SIZE);
    }
}

/* CHG_Status's flags byte, bits 0-4, as can/control.dbc orders them. */
static uint8_t flags_of(const struct can_control_chg_status_t *s)
{
    return (uint8_t)((s->hardware_failure & 1u) | ((s->over_temperature & 1u) << 1) |
                     ((s->input_voltage_wrong & 1u) << 2) | ((s->starting_state & 1u) << 3) |
                     ((s->comm_timeout & 1u) << 4));
}

/* The current the charger has been asked for now: the last request's, or
   nothing once the charger would have stopped by itself. */
static uint16_t asked_da(const bms_charge_t *c, uint32_t now_ms)
{
    if (!c->requested || (uint32_t)(now_ms - c->request_ms) > BMS_CHARGE_CHARGER_STOPS_MS) {
        return 0u;
    }
    return c->request_da;
}

bms_charge_heard_t bms_charge_listen(bms_charge_t *c, uint32_t now_ms, const mailbox_t *charger,
                                     bms_charge_events_t *events)
{
    const mailbox_config_t *config = &control_rx_config[CONTROL_RX_CHG_STATUS];
    struct can_control_chg_status_t s;

    /* Not VALID reads the Substitute: 0 V, 0 A, no flags. */
    (void)can_control_chg_status_unpack(&s, mailbox_read(charger, config), config->length);

    if (!c->charge_mode && charger->counters.good != 0u) {
        c->charge_mode = true;
        c->state = BMS_CHARGE_WAITING;
    }
    c->heard = charger->state == MAILBOX_VALID;

    /* Only a frame the charger sent changes its flags; silence does not. */
    if (c->heard) {
        const uint8_t flags = flags_of(&s);
        if (flags != c->flags) {
            const event_charger_flags_t e = {
                .from = c->flags,
                .to = flags,
                .charge_state = (uint8_t)c->state,
                .output_dv = s.output_voltage,
                .output_da = s.output_current,
            };
            uint8_t data[EVENT_DATA_SIZE];
            event_data_charger_flags(data, &e);
            add_event(events, EVENT_CHARGER_FLAGS, data);
            c->flags = flags;
        }
    }

    const bool over =
        (uint32_t)s.output_current > (uint32_t)asked_da(c, now_ms) + BMS_CHARGE_DISOBEY_MARGIN_DA;
    if (over && !c->over) {
        c->over_since_ms = now_ms;
    }
    c->over = over;

    bms_charge_obey_t obey = BMS_CHARGE_OBEYS;
    if (over) {
        obey = ((uint32_t)(now_ms - c->over_since_ms) >= BMS_CHARGE_DISOBEY_MS)
                   ? BMS_CHARGE_DISOBEYS
                   : BMS_CHARGE_OVER;
    }
    return (bms_charge_heard_t){
        .charge_mode = c->charge_mode,
        .obey = obey,
        .reported_da = s.output_current,
    };
}

/* A linear taper on a value that rises toward trouble: FULL_PPM at or
   below full, 0 at or above zero, a straight line between. full < zero
   (the parameter bounds keep them apart). */
static uint32_t taper(int32_t x, int32_t full, int32_t zero)
{
    if (x <= full) {
        return FULL_PPM;
    }
    if (x >= zero) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)FULL_PPM * (uint32_t)(zero - x)) / (uint32_t)(zero - full));
}

static uint32_t lowest(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint32_t target_ppm(const bms_charge_config_t *k, const bms_pack_summary_t *p)
{
    const uint32_t v = taper(p->max_mv, (int32_t)k->target_mv - (int32_t)k->v_band_mv,
                             k->target_mv);
    const uint32_t hot = taper(p->max_dc, k->hot_full_dc, k->hot_zero_dc);
    /* Cold: trouble lies downward, so taper on the negative. */
    const uint32_t cold = taper(-(int32_t)p->min_dc, -(int32_t)k->cold_full_dc,
                                -(int32_t)k->cold_zero_dc);
    return lowest(v, lowest(hot, cold));
}

static event_charge_cells_t cells_of(const bms_pack_summary_t *p)
{
    if (p->cells == 0u) {
        return (event_charge_cells_t){0};
    }
    return (event_charge_cells_t){
        .highest_mv = p->max_mv,
        .highest = (uint16_t)(p->max_cell + 1u),
        .lowest_mv = p->min_mv,
        .lowest = (uint16_t)(p->min_cell + 1u),
    };
}

static void change(bms_charge_t *c, bms_charge_state_t to, bms_charge_cause_t cause,
                   const bms_charge_in_t *in, uint16_t limit_da, bms_charge_events_t *events)
{
    const event_charge_t e = {
        .from = (uint8_t)c->state,
        .to = (uint8_t)to,
        .cause = (uint8_t)cause,
        .cells = cells_of(in->summary),
        .limit_da = limit_da,
    };
    uint8_t data[EVENT_DATA_SIZE];
    event_data_charge(data, &e);
    add_event(events, EVENT_CHARGE, data);

    c->state = to;
    c->starting = false;
    c->ending = false;
    if (to == BMS_CHARGE_COMPLETE) {
        c->complete_ms = in->now_ms;
    }
}

/* Whether cond has held, without a break, for hold_ms. */
static bool held(bool cond, bool *timing, uint32_t *since_ms, uint32_t now_ms, uint32_t hold_ms)
{
    if (!cond) {
        *timing = false;
        return false;
    }
    if (!*timing) {
        *timing = true;
        *since_ms = now_ms;
    }
    return (uint32_t)(now_ms - *since_ms) >= hold_ms;
}

/* Why a charge cannot go on, or BMS_CHARGE_CAUSE_NONE. */
static bms_charge_cause_t blocked(const bms_charge_t *c, const bms_charge_in_t *in)
{
    if (!c->heard) {
        return BMS_CHARGE_CAUSE_SILENT;
    }
    if (in->fault_active || !in->bms_ok) {
        return BMS_CHARGE_CAUSE_BMS_NOT_OK;
    }
    if (!in->sdc_closed) {
        return BMS_CHARGE_CAUSE_SENSE_OPEN;
    }
    return BMS_CHARGE_CAUSE_NONE;
}

uint16_t bms_charge_step(bms_charge_t *c, const bms_charge_in_t *in, bms_charge_events_t *events)
{
    const bms_charge_config_t *k = &c->config;
    const bms_pack_summary_t *p = in->summary;

    if (in->swept) {
        const uint32_t target = target_ppm(k, p);
        c->fraction_ppm = c->swept ? bms_derate_rate_limited(c->fraction_ppm, target,
                                                             in->now_ms - c->last_sweep_ms)
                                   : target; /* the first: at once */
        c->swept = true;
        c->last_sweep_ms = in->now_ms;
    }

    uint16_t limit = 0u;
    if (c->charge_mode && in->bms_ok && c->state != BMS_CHARGE_COMPLETE) {
        limit = (uint16_t)(((uint64_t)k->full_limit_da * c->fraction_ppm) / FULL_PPM);
    }

    switch (c->state) {
    case BMS_CHARGE_WAITING: {
        const bool ready = blocked(c, in) == BMS_CHARGE_CAUSE_NONE && limit > 0u;
        if (held(ready, &c->starting, &c->start_since_ms, in->now_ms, BMS_CHARGE_START_MS)) {
            change(c, BMS_CHARGE_CHARGING, BMS_CHARGE_CAUSE_NONE, in, limit, events);
        }
        break;
    }
    case BMS_CHARGE_CHARGING: {
        const bms_charge_cause_t cause = blocked(c, in);
        const bool atTarget = p->cells != 0u && p->max_mv >= k->target_mv;
        if (cause != BMS_CHARGE_CAUSE_NONE) {
            change(c, BMS_CHARGE_WAITING, cause, in, limit, events);
        } else if (held(limit < BMS_CHARGE_END_LIMIT_DA && atTarget, &c->ending,
                        &c->end_since_ms, in->now_ms, BMS_CHARGE_END_MS)) {
            change(c, BMS_CHARGE_COMPLETE, BMS_CHARGE_CAUSE_TARGET, in, limit, events);
            limit = 0u;
        }
        break;
    }
    case BMS_CHARGE_COMPLETE:
        if (!c->rested_logged &&
            (uint32_t)(in->now_ms - c->complete_ms) >= BMS_CHARGE_RESTED_MS) {
            const event_charge_cells_t cells = cells_of(p);
            uint8_t data[EVENT_DATA_SIZE];
            event_data_charge_rested(data, &cells, in->now_ms - c->complete_ms);
            add_event(events, EVENT_CHARGE_RESTED, data);
            c->rested_logged = true;
        }
        break;
    default: /* OFF: in the car */
        break;
    }

    c->limit_da = limit;
    return limit;
}

void bms_charge_request(bms_charge_t *c, uint32_t now_ms, struct can_control_chg_request_t *req)
{
    const bool charging = c->state == BMS_CHARGE_CHARGING;

    /* 128 x the target, mV to 0.1 V. */
    *req = (struct can_control_chg_request_t){
        .max_charge_voltage =
            charging ? (uint16_t)(((uint32_t)c->config.target_mv * BMS_CHARGE_PACK_CELLS) / 100u)
                     : 0u,
        .max_charge_current = charging ? c->limit_da : 0u,
        .control = charging ? CAN_CONTROL_CHG_REQUEST_CONTROL_START_CHOICE
                            : CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE,
    };
    c->requested = true;
    c->request_ms = now_ms;
    c->request_da = req->max_charge_current;
}

bms_charge_state_t bms_charge_state(const bms_charge_t *c)
{
    return c->state;
}

const char *bms_charge_state_name(bms_charge_state_t state)
{
    switch (state) {
    case BMS_CHARGE_OFF:      return "OFF";
    case BMS_CHARGE_WAITING:  return "WAITING";
    case BMS_CHARGE_CHARGING: return "CHARGING";
    case BMS_CHARGE_COMPLETE: return "COMPLETE";
    default:                  return "?";
    }
}
