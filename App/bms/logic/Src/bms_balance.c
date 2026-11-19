/* bms_balance.c — balancing, see bms_balance.h. */

#include "bms_balance.h"

#include <string.h>

#include "cell_temp.h"
#include "event_codes.h"

void bms_balance_init(bms_balance_t *b, uint16_t target_mv)
{
    memset(b, 0, sizeof *b);
    b->target_mv = target_mv;
    b->state = BMS_BALANCE_OFF;
    b->reason = BMS_BALANCE_NONE;
    b->sent.op = BQ_BALANCE_NONE;
    /* The BMS starts in STARTUP, not CHARGE: the interlock's 3 s count
       from the first step it holds. */
    b->interlock_reason = BMS_BALANCE_BMS_FAULT;
}

static void add_event(bms_balance_out_t *out, const event_balancing_t *e)
{
    if (out->event_count < BMS_BALANCE_EVENTS_MAX) {
        bms_balance_event_t *ev = &out->events[out->event_count++];
        ev->code = EVENT_BALANCING;
        event_data_balancing(ev->data, e);
    }
}

/* Moves to state with reason; an Event if either changed. */
static void change(bms_balance_t *b, bms_balance_out_t *out, uint8_t state, uint8_t reason,
                   uint16_t detail)
{
    if (state == b->state && reason == b->reason) {
        return;
    }
    const event_balancing_t e = {
        .from = b->state,
        .to = state,
        .reason = reason,
        .spread_mv = b->extremes ? b->spread_mv : 0u,
        .cells = b->extremes ? (event_charge_cells_t){
            .highest_mv = b->max_mv,
            .highest = (uint16_t)(b->max_cell + 1u),
            .lowest_mv = b->min_mv,
            .lowest = (uint16_t)(b->min_cell + 1u),
        } : (event_charge_cells_t){0},
        .detail = detail,
    };
    add_event(out, &e);
    b->state = state;
    b->reason = reason;
}

/* STOPPED until a power cycle: every FET off, nothing more chosen. */
static void stop_for_good(bms_balance_t *b, bms_balance_out_t *out, uint8_t reason,
                          uint16_t detail)
{
    change(b, out, BMS_BALANCE_STOPPED, reason, detail);
    b->set_due = false;
    b->fet_stage = BMS_FET_CHECK_IDLE;
    b->stop_due = b->stop_due || b->maybe_on;
}

static void fets_off_at(bms_balance_t *b, uint32_t ms)
{
    b->maybe_on = false;
    b->stop_due = false;
    b->ever_off = true;
    b->off_ms = ms;
    memset(b->on, 0, sizeof b->on);
}

/* The FET check given up for now: the next decision runs it again. */
static void abandon_fet_check(bms_balance_t *b)
{
    b->fet_stage = BMS_FET_CHECK_IDLE;
    b->stop_due = b->stop_due || b->maybe_on;
}

/* What last step's command did. */
static void apply_result(bms_balance_t *b, const bms_balance_in_t *in, bms_balance_out_t *out)
{
    const bq_balance_op_t op = b->sent.op;
    const bq_balance_result_t *r = in->result;
    const bool ok = in->ran && r != NULL && r->ok;

    b->sent.op = BQ_BALANCE_NONE;
    switch (op) {
    case BQ_BALANCE_STOP:
        if (ok) {
            fets_off_at(b, b->sent_ms);
            b->die_read = r->devices_read;
            memcpy(b->die_dc, r->die_dc, sizeof b->die_dc);
        } else {
            b->die_read = 0u;
            b->stop_due = b->stop_due || b->maybe_on;
        }
        break;
    case BQ_BALANCE_SET:
        if (!ok) {
            b->stop_due = true; /* which FETs are on is not known */
        } else if (r->rejected != 0u) {
            stop_for_good(b, out, BMS_BALANCE_CONFIG_REJECTED, r->rejected);
            b->stop_due = true;
        }
        break;
    case BQ_BALANCE_FET_ON:
    case BQ_BALANCE_FET_COMPARE:
        if (!ok) {
            abandon_fet_check(b);
        }
        break;
    case BQ_BALANCE_FET_READ:
        if (ok) {
            fets_off_at(b, b->sent_ms); /* the off went before the reads */
        }
        if (r != NULL && in->ran && r->fet == BQ_FET_CHECK_FAIL) {
            const uint16_t fet = (r->fet_cell == 0u)
                                     ? 0u
                                     : (uint16_t)((r->fet_cell - 1u) % BQ_SWEEP_CELLS_PER_DEVICE + 1u);
            stop_for_good(b, out, BMS_BALANCE_FET_CHECK_FAILED,
                          (uint16_t)((r->fet_device << 8) | fet));
        } else if (ok && r->fet == BQ_FET_CHECK_PASS) {
            if (b->fet_cells == BMS_BALANCE_ODD_CELLS) {
                b->fet_cells = BMS_BALANCE_EVEN_CELLS;
                b->fet_stage = BMS_FET_CHECK_DUE;
            } else {
                b->fet_checked = true;
                b->fet_stage = BMS_FET_CHECK_IDLE;
            }
        } else {
            abandon_fet_check(b);
        }
        break;
    default:
        break;
    }
}

static bool sweep_whole(const bq_sweep_t *s)
{
    if (s->devices_read != (uint16_t)((1u << BQ_SWEEP_DEVICES) - 1u)) {
        return false;
    }
    for (size_t i = 0u; i < BQ_SWEEP_CELLS; i++) {
        if (s->cell_mv[i] == 0u) {
            return false;
        }
    }
    return true;
}

static void take_extremes(bms_balance_t *b, const bq_sweep_t *s)
{
    b->extremes = true;
    b->max_mv = 0u;
    b->min_mv = UINT16_MAX;
    for (size_t i = 0u; i < BQ_SWEEP_CELLS; i++) {
        const uint16_t v = s->cell_mv[i];
        if (v > b->max_mv) {
            b->max_mv = v;
            b->max_cell = (uint8_t)i;
        }
        if (v < b->min_mv) {
            b->min_mv = v;
            b->min_cell = (uint8_t)i;
        }
    }
    b->spread_mv = (uint16_t)(b->max_mv - b->min_mv);
}

/* The hottest sensor, or false if any reading is invalid. */
static bool hottest(const bq_sweep_t *s, int16_t *dc)
{
    bool any = false;
    for (size_t i = 0u; i < BQ_SWEEP_CELLS; i++) {
        int16_t t;
        if (!cell_temp_from_mv(s->temp_mv[i], &t)) {
            return false;
        }
        if (!any || t > *dc) {
            *dc = t;
            any = true;
        }
    }
    return any;
}

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

/* The interlock's first false condition, or BMS_BALANCE_NONE. */
static uint8_t interlock_false(const bms_balance_in_t *in)
{
    if (!in->sdc_closed) {
        return BMS_BALANCE_SENSE_OPEN;
    }
    if (!in->state_charge || !in->bms_ok || in->fault_active) {
        return BMS_BALANCE_BMS_FAULT;
    }
    if (!in->charger_heard) {
        return BMS_BALANCE_CHARGER_SILENT;
    }
    return BMS_BALANCE_NONE;
}

/* The decision, on a settled Sweep that read every cell (bms_balance.h),
   whose extremes take_extremes() has just taken. */
static void decide(bms_balance_t *b, const bms_balance_in_t *in, bms_balance_out_t *out)
{
    const bq_sweep_t *s = in->sweep;
    int16_t hot = 0;

    if (!hottest(s, &hot)) {
        return; /* a sensor not read validly: nothing chosen this second */
    }
    const uint16_t dieRead = b->die_read;
    b->die_read = 0u; /* each decision takes its own pause's */

    if ((uint32_t)b->min_mv + BMS_BALANCE_LOW_CELL_MV < b->target_mv) {
        stop_for_good(b, out, BMS_BALANCE_LOW_CELL, 0u);
        return;
    }

    b->cells_hot = b->cells_hot ? hot >= BMS_BALANCE_CELLS_COOL_DC
                                : hot >= BMS_BALANCE_CELLS_HOT_DC;
    for (uint8_t d = 0u; d < BQ_SWEEP_DEVICES; d++) {
        const uint16_t bit = (uint16_t)(1u << d);
        if ((dieRead & bit) == 0u) {
            continue; /* not read: it does not bleed, its flag stands */
        }
        const bool wasHot = (b->board_hot & bit) != 0u;
        const bool isHot = wasHot ? b->die_dc[d] > BMS_BALANCE_BOARD_COOL_DC
                                  : b->die_dc[d] >= BMS_BALANCE_BOARD_HOT_DC;
        b->board_hot = isHot ? (uint16_t)(b->board_hot | bit) : (uint16_t)(b->board_hot & ~bit);
    }
    if (b->cells_hot) {
        change(b, out, BMS_BALANCE_PAUSED, BMS_BALANCE_CELLS_HOT, 0u);
        return;
    }

    if (!b->session && b->spread_mv > BMS_BALANCE_START_SPREAD_MV) {
        b->session = true;
    } else if (b->session && b->spread_mv <= BMS_BALANCE_DONE_SPREAD_MV) {
        b->session = false;
    }
    if (!b->session) {
        change(b, out, BMS_BALANCE_DONE, BMS_BALANCE_NONE, 0u);
        return;
    }
    if (!b->fet_checked) {
        b->fet_stage = BMS_FET_CHECK_DUE;
        b->fet_cells = BMS_BALANCE_ODD_CELLS;
        change(b, out, BMS_BALANCE_BALANCING, BMS_BALANCE_NONE, 0u);
        return;
    }

    b->odd = !b->odd;
    const uint8_t mine = b->odd ? BMS_BALANCE_ODD_CELLS : BMS_BALANCE_EVEN_CELLS;
    bool anySet = false;
    bool anyHot = false;
    memset(b->set, 0, sizeof b->set);
    for (uint8_t d = 0u; d < BQ_SWEEP_DEVICES; d++) {
        const uint16_t bit = (uint16_t)(1u << d);
        uint8_t chosen = 0u;
        for (uint8_t k = 0u; k < BQ_SWEEP_CELLS_PER_DEVICE; k++) {
            const uint16_t v = s->cell_mv[d * BQ_SWEEP_CELLS_PER_DEVICE + k];
            if ((uint32_t)v > (uint32_t)b->min_mv + BMS_BALANCE_BLEED_ABOVE_MV) {
                chosen |= (uint8_t)(1u << k);
            }
        }
        if (chosen == 0u) {
            continue;
        }
        if ((b->board_hot & bit) != 0u) {
            anyHot = true;
            continue;
        }
        if ((dieRead & bit) == 0u) {
            continue;
        }
        uint8_t set = chosen & mine;
        if (set == 0u) {
            set = chosen & (uint8_t)~mine;
        }
        b->set[d] = set;
        anySet = true;
    }
    b->set_due = anySet;
    if (!anySet && anyHot) {
        change(b, out, BMS_BALANCE_PAUSED, BMS_BALANCE_BOARD_HOT, 0u);
    } else {
        change(b, out, BMS_BALANCE_BALANCING, BMS_BALANCE_NONE, 0u);
    }
}

static void send(bms_balance_t *b, bms_balance_out_t *out, bq_balance_op_t op, uint32_t now_ms)
{
    out->command.op = op;
    b->sent = out->command;
    b->sent_ms = now_ms;
}

/* While balancing may run: the set, the FET check's next stage, the
   pause's stop or the decision, whichever is due. */
static void run(bms_balance_t *b, const bms_balance_in_t *in, bool whole,
                bms_balance_out_t *out)
{
    const uint32_t now = in->now_ms;

    if (b->set_due) {
        b->set_due = false;
        memcpy(out->command.cells, b->set, sizeof b->set);
        memcpy(b->on, b->set, sizeof b->set);
        b->maybe_on = true;
        send(b, out, BQ_BALANCE_SET, now);
        return;
    }

    switch (b->fet_stage) {
    case BMS_FET_CHECK_DUE:
        out->command.fet_cells = b->fet_cells;
        b->fet_stage = BMS_FET_CHECK_ON;
        b->fet_stage_ms = now;
        b->maybe_on = true;
        send(b, out, BQ_BALANCE_FET_ON, now);
        return;
    case BMS_FET_CHECK_ON:
        if ((uint32_t)(now - b->fet_stage_ms) >= BMS_BALANCE_FET_SETTLE_MS) {
            b->fet_stage = BMS_FET_CHECK_COMPARE;
            b->fet_stage_ms = now;
            send(b, out, BQ_BALANCE_FET_COMPARE, now);
        }
        return;
    case BMS_FET_CHECK_COMPARE:
        if ((uint32_t)(now - b->fet_stage_ms) >= BMS_BALANCE_FET_COMPARE_MS) {
            b->fet_stage = BMS_FET_CHECK_READ;
            out->command.fet_cells = b->fet_cells;
            send(b, out, BQ_BALANCE_FET_READ, now);
        }
        return;
    case BMS_FET_CHECK_READ: /* its result comes in on the next step */
        return;
    default:
        break;
    }

    const bool pauseDue =
        in->checked &&
        (uint32_t)(now - in->last_check_ms) >= BMS_BALANCE_PERIOD_MS - BMS_BALANCE_PAUSE_MS &&
        !(b->paused && b->paused_for_ms == in->last_check_ms);
    if (pauseDue) {
        b->paused = true;
        b->paused_for_ms = in->last_check_ms;
        send(b, out, BQ_BALANCE_STOP, now);
        return;
    }

    if (in->check_due && out->settled && whole) {
        decide(b, in, out);
    }
}

void bms_balance_step(bms_balance_t *b, const bms_balance_in_t *in, bms_balance_out_t *out)
{
    memset(out, 0, sizeof *out);
    out->command.op = BQ_BALANCE_NONE;

    apply_result(b, in, out);

    out->settled = in->swept && !b->maybe_on &&
                   (!b->ever_off || (uint32_t)(in->now_ms - b->off_ms) >= BMS_BALANCE_PAUSE_MS);
    const bool whole = out->settled && sweep_whole(in->sweep);
    if (whole) {
        take_extremes(b, in->sweep);
        if (in->charge_mode && (uint32_t)b->max_mv + BMS_BALANCE_GATE_MV >= b->target_mv) {
            b->gate = true;
        }
    }

    /* The time limit, counted from Charge complete through every pause. */
    if (in->charge_complete && !b->complete_seen) {
        b->complete_seen = true;
        b->complete_ms = in->now_ms;
    }
    if (b->complete_seen && !b->ended &&
        (uint32_t)(in->now_ms - b->complete_ms) >= BMS_BALANCE_TIME_LIMIT_MS) {
        b->ended = true;
        if (b->state != BMS_BALANCE_DONE && b->state != BMS_BALANCE_STOPPED) {
            stop_for_good(b, out, BMS_BALANCE_TIME_LIMIT, 0u);
        }
    }

    /* The interlock (EV.8.3.3). */
    const uint8_t falseNow = in->charge_mode ? interlock_false(in) : BMS_BALANCE_NONE;
    if (falseNow != BMS_BALANCE_NONE) {
        b->interlock_reason = falseNow;
    }
    const bool interlock = held(in->charge_mode && falseNow == BMS_BALANCE_NONE,
                                &b->allowed_timing, &b->allowed_since_ms, in->now_ms,
                                BMS_BALANCE_RESTART_MS);

    const bool may_run = in->charge_mode && b->gate && b->state != BMS_BALANCE_STOPPED &&
                         !b->ended && interlock;
    if (!in->charge_mode) {
        change(b, out, BMS_BALANCE_OFF, BMS_BALANCE_NONE, 0u);
    } else if (b->state == BMS_BALANCE_STOPPED || b->ended) {
        /* final: STOPPED, or DONE when the time ran out */
    } else if (!b->gate) {
        change(b, out, BMS_BALANCE_WAITING, BMS_BALANCE_NONE, 0u);
    } else if (!interlock) {
        change(b, out, BMS_BALANCE_PAUSED, b->interlock_reason, 0u);
    }
    if (!may_run) {
        b->set_due = false;
        if (b->fet_stage != BMS_FET_CHECK_IDLE) {
            abandon_fet_check(b);
        }
    }

    if (b->stop_due || (!may_run && b->maybe_on)) {
        send(b, out, BQ_BALANCE_STOP, in->now_ms); /* in the same step */
    } else if (may_run) {
        run(b, in, whole, out);
    }

    out->hold_check = out->command.op != BQ_BALANCE_NONE || b->fet_stage != BMS_FET_CHECK_IDLE;
    out->state = b->state;
    out->reason = b->reason;
    out->spread_mv = b->extremes ? b->spread_mv : 0u;
    memcpy(out->on, b->on, sizeof out->on);
}

bool bms_balance_stopped(const bms_balance_t *b)
{
    return b->state == BMS_BALANCE_STOPPED;
}

const char *bms_balance_state_name(uint8_t state)
{
    switch (state) {
    case BMS_BALANCE_OFF:       return "OFF";
    case BMS_BALANCE_WAITING:   return "WAITING_FOR_GATE";
    case BMS_BALANCE_BALANCING: return "BALANCING";
    case BMS_BALANCE_PAUSED:    return "PAUSED";
    case BMS_BALANCE_DONE:      return "DONE";
    case BMS_BALANCE_STOPPED:   return "STOPPED";
    default:                    return "?";
    }
}

const char *bms_balance_reason_name(uint8_t reason)
{
    switch (reason) {
    case BMS_BALANCE_NONE:             return "NONE";
    case BMS_BALANCE_SENSE_OPEN:       return "SENSE_OPEN";
    case BMS_BALANCE_BMS_FAULT:        return "BMS_FAULT";
    case BMS_BALANCE_CHARGER_SILENT:   return "CHARGER_SILENT";
    case BMS_BALANCE_BOARD_HOT:        return "BOARD_HOT";
    case BMS_BALANCE_CELLS_HOT:        return "CELLS_HOT";
    case BMS_BALANCE_LOW_CELL:         return "LOW_CELL";
    case BMS_BALANCE_TIME_LIMIT:       return "TIME_LIMIT";
    case BMS_BALANCE_FET_CHECK_FAILED: return "FET_CHECK_FAILED";
    case BMS_BALANCE_CONFIG_REJECTED:  return "CONFIG_REJECTED";
    default:                           return "?";
    }
}
