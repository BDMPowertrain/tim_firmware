/* bms_control.c — the BMS's Control step as one function, see
   bms_control.h. */

#include "bms_control.h"

#include <string.h>

#include "bms_tx.h"
#include "cell_temp.h"
#include "event_codes.h"

/* What one step's Sweep says about one fault. */
typedef enum {
    VERDICT_CLEAN,   /* judged, and in range */
    VERDICT_BAD,     /* judged, and out of range */
    VERDICT_UNKNOWN, /* not judged: a reading it needs is missing or invalid */
} verdict_t;

typedef struct {
    verdict_t verdict;
    uint8_t   where;  /* EVENT_BMS_... of the worst reading */
    uint16_t  index;  /* 1 to 128 */
    uint16_t  mv;
} judged_t;

/* Bad Sweeps in a row to raise each fault, by bms_fault_t; 0: not judged
   here. The open-wire and internal faults count their own way (their
   checks, or the self-checks' own counts) and are bad only once those
   have run out. */
static const uint16_t raiseAt[BMS_FAULT_COUNT] = {
    [BMS_FAULT_OVER_VOLTAGE]  = BMS_OV_SWEEPS,
    [BMS_FAULT_UNDER_VOLTAGE] = BMS_UV_SWEEPS,
    [BMS_FAULT_OVER_TEMP]     = BMS_TEMP_SWEEPS,
    [BMS_FAULT_UNDER_TEMP]    = BMS_TEMP_SWEEPS,
    [BMS_FAULT_MEASUREMENT]   = BMS_MEAS_SWEEPS,
    [BMS_FAULT_OPEN_WIRE]     = 1u,
    [BMS_FAULT_CELL_MONITOR]  = BMS_CELL_MONITOR_SWEEPS,
    [BMS_FAULT_INTERNAL]      = 1u,
    [BMS_FAULT_CHARGER_DISOBEYS] = 1u, /* bms_charge.h times the 5 s */
};

bool bms_control_held(reset_cause_t cause, bool debugger)
{
    return !debugger && !reset_cause_is_power_on(cause);
}

void bms_control_init(bms_control_t *bc, const bms_control_config_t *config)
{
    memset(bc, 0, sizeof *bc);
    bc->config = *config;
    bc->state = BMS_STATE_STARTUP;
    bms_derate_init(&bc->derate, &config->derate);
    bms_charge_init(&bc->charge, &config->charge);
    bms_balance_init(&bc->balance, config->charge.target_mv);
}

/* The balancing second is the open-wire check's (bms_balance.h). */
_Static_assert(BMS_BALANCE_PERIOD_MS == BMS_OPEN_WIRE_PERIOD_MS,
               "balancing decides on the open-wire check's Sweep");

static uint16_t bit(bms_fault_t f)
{
    return (uint16_t)(1u << f);
}

static void count_up(uint16_t *n)
{
    if (*n < UINT16_MAX) {
        (*n)++;
    }
}

/* The worst reading for one fault: kept if worse (by worse()) than the
   last, and bad if past the threshold. */
static void consider(judged_t *j, uint8_t where, size_t i, uint16_t mv, bool bad, bool worse)
{
    if (j->index == 0u || worse) {
        j->where = where;
        j->index = (uint16_t)(i + 1u);
        j->mv = mv;
    }
    if (bad) {
        j->verdict = VERDICT_BAD;
    }
}

static void unknown_unless_bad(judged_t *j)
{
    if (j->verdict != VERDICT_BAD) {
        j->verdict = VERDICT_UNKNOWN;
    }
}

/* Judges this step's Sweep for every fault. A fault this function does not
   judge (#64, #65, #67, overcurrent) is clean. */
static void judge(const bms_control_in_t *in, bool charge_mode, bool started_up,
                  judged_t j[BMS_FAULT_COUNT])
{
    memset(j, 0, sizeof(judged_t) * BMS_FAULT_COUNT);

    if (!in->swept) {
        /* No Sweep: nothing to judge a cell or sensor by. After STARTUP it
           is chain comms loss, a missing measurement; in STARTUP the chain
           is still waking (the silent timer covers it). */
        j[BMS_FAULT_OVER_VOLTAGE].verdict = VERDICT_UNKNOWN;
        j[BMS_FAULT_UNDER_VOLTAGE].verdict = VERDICT_UNKNOWN;
        j[BMS_FAULT_OVER_TEMP].verdict = VERDICT_UNKNOWN;
        j[BMS_FAULT_UNDER_TEMP].verdict =
            charge_mode ? VERDICT_UNKNOWN : VERDICT_CLEAN;
        j[BMS_FAULT_MEASUREMENT].verdict = started_up ? VERDICT_BAD : VERDICT_UNKNOWN;
        return;
    }

    const bq_sweep_t *s = in->sweep;
    judged_t *meas = &j[BMS_FAULT_MEASUREMENT];
    bool cellMissing = false;
    bool tempMissing = false;

    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        const bool read = (s->devices_read & (1u << (i / BQ_SWEEP_CELLS_PER_DEVICE))) != 0u;
        const uint16_t v = s->cell_mv[i];
        const uint16_t t = s->temp_mv[i];

        if (!read || v == 0u) {
            cellMissing = true;
            if (meas->verdict != VERDICT_BAD) {
                consider(meas, EVENT_BMS_CELL, i, v, true, true);
            }
        } else {
            judged_t *ov = &j[BMS_FAULT_OVER_VOLTAGE];
            judged_t *uv = &j[BMS_FAULT_UNDER_VOLTAGE];
            consider(ov, EVENT_BMS_CELL, i, v, v >= BMS_OV_MV, v > ov->mv);
            consider(uv, EVENT_BMS_CELL, i, v, v <= BMS_UV_MV, v < uv->mv);
        }

        if (!read || t < CELL_TEMP_MV_MIN || t > CELL_TEMP_MV_MAX) {
            tempMissing = true;
            if (meas->verdict != VERDICT_BAD) {
                consider(meas, EVENT_BMS_SENSOR, i, t, true, true);
            }
        } else {
            /* The reading falls as the cell warms. */
            judged_t *ot = &j[BMS_FAULT_OVER_TEMP];
            judged_t *ut = &j[BMS_FAULT_UNDER_TEMP];
            consider(ot, EVENT_BMS_SENSOR, i, t, t <= BMS_OT_MV, t < ot->mv);
            consider(ut, EVENT_BMS_SENSOR, i, t, t >= BMS_UT_MV, t > ut->mv);
        }
    }

    /* A fault not bad on what was read is unknown if anything it needs was
       not read: it is neither raised nor cleared on this Sweep. */
    if (cellMissing) {
        unknown_unless_bad(&j[BMS_FAULT_OVER_VOLTAGE]);
        unknown_unless_bad(&j[BMS_FAULT_UNDER_VOLTAGE]);
    }
    if (tempMissing) {
        unknown_unless_bad(&j[BMS_FAULT_OVER_TEMP]);
        unknown_unless_bad(&j[BMS_FAULT_UNDER_TEMP]);
    }
    /* Under-temperature is a fault in Charge mode only. */
    if (!charge_mode) {
        j[BMS_FAULT_UNDER_TEMP].verdict = VERDICT_CLEAN;
    }
}

/* Charger disobeys (issue 67): bms_charge_listen() has timed it. */
static void judge_charger(const bms_charge_heard_t *h, judged_t *j)
{
    switch (h->obey) {
    case BMS_CHARGE_DISOBEYS:
        *j = (judged_t){ .verdict = VERDICT_BAD, .where = EVENT_BMS_CHARGER,
                         .mv = h->reported_da };
        break;
    case BMS_CHARGE_OVER: /* timing: neither raised nor clean */
        j->verdict = VERDICT_UNKNOWN;
        break;
    default:
        j->verdict = VERDICT_CLEAN;
        break;
    }
}

/* The open-wire check handed in from the last step (issue 64). Bad only on
   the check that makes BMS_OPEN_WIRE_CHECKS failures in a row; clean while
   the last check that judged passed; otherwise unknown. */
static void judge_open_wire(bms_control_t *bc, const bms_control_in_t *in, judged_t *j)
{
    bool raise = false;

    if (in->open_wire_ran) {
        switch (in->open_wire.verdict) {
        case BQ_OPEN_WIRE_PASS:
            bc->ow_fails = 0u;
            bc->ow_passed = true;
            break;
        case BQ_OPEN_WIRE_FAIL:
            bc->ow_passed = false;
            bc->ow_failed = in->open_wire;
            if (bc->ow_fails < UINT8_MAX) {
                bc->ow_fails++;
            }
            raise = bc->ow_fails >= BMS_OPEN_WIRE_CHECKS;
            break;
        default: /* could not judge: the run neither grows nor ends */
            break;
        }
    }

    if (raise) {
        const bq_open_wire_t *f = &bc->ow_failed;
        *j = (judged_t){
            .verdict = VERDICT_BAD,
            .where = (f->cell != 0u) ? EVENT_BMS_CELL : EVENT_BMS_DEVICE,
            .index = (f->cell != 0u) ? f->cell : f->device,
        };
    } else {
        j->verdict = bc->ow_passed ? VERDICT_CLEAN : VERDICT_UNKNOWN;
    }
}

/* NFAULT and the bridge's FAULT_SUMMARY (issue 64). NFAULT means nothing
   before the chain has been woken and cleared, so in STARTUP it is judged
   only with a Sweep. */
static void judge_cell_monitor(const bms_control_in_t *in, bool started_up, judged_t *j)
{
    const bool registerRead = in->swept && in->sweep->bridge_read;
    const uint8_t summary = registerRead ? in->sweep->bridge_fault : 0u;

    if (!in->nfault_ok && (in->swept || started_up)) {
        *j = (judged_t){ .verdict = VERDICT_BAD, .where = EVENT_BMS_NFAULT, .mv = summary };
    } else if (registerRead && summary != 0u) {
        *j = (judged_t){ .verdict = VERDICT_BAD, .where = EVENT_BMS_BRIDGE, .mv = summary };
    } else {
        j->verdict = registerRead ? VERDICT_CLEAN : VERDICT_UNKNOWN;
    }
}

/* The BMS's own output path (issue 64; spec #36, the self-checks), and
   its own timing (issue 65). */
static void judge_internal(bms_control_t *bc, const bms_control_in_t *in, judged_t *j)
{
    const bool registerRead = in->swept && in->sweep->bridge_read;
    const uint8_t summary = registerRead ? in->sweep->bridge_fault : 0u;

    /* BMS_OK is the AND of MCU_BMS_OK and NFAULT (ADR 0008). Counted in
       Sweeps (spec #36: "a mismatch for 5 sweeps"): a step that took none
       neither counts nor ends a run. */
    const bool expected = bc->commanded && in->nfault_ok;
    if (in->swept) {
        if (in->bms_ok_fb != expected) {
            count_up(&bc->feedback_bad);
        } else {
            bc->feedback_bad = 0u;
        }
    }

    if (registerRead) {
        if (in->nfault_ok != (summary == 0u)) {
            count_up(&bc->nfault_check_bad);
        } else {
            bc->nfault_check_bad = 0u;
        }
    }

    uint32_t lowMs = 0u;
    if (!in->bms_ok_fb && in->sdc_closed) {
        if (!bc->sense_timing) {
            bc->sense_timing = true;
            bc->sense_since_ms = in->now_ms;
        }
        lowMs = in->now_ms - bc->sense_since_ms;
    } else {
        bc->sense_timing = false;
    }

    if (in->timing_fault) {
        *j = (judged_t){ .verdict = VERDICT_BAD, .where = EVENT_BMS_TIMING };
    } else if (bc->feedback_bad >= BMS_FEEDBACK_SWEEPS) {
        const uint16_t bits = (uint16_t)((in->bms_ok_fb ? 1u : 0u) | (bc->commanded ? 2u : 0u) |
                                         (in->nfault_ok ? 4u : 0u));
        *j = (judged_t){ .verdict = VERDICT_BAD, .where = EVENT_BMS_FEEDBACK, .mv = bits };
    } else if (bc->nfault_check_bad >= BMS_FEEDBACK_SWEEPS) {
        *j = (judged_t){ .verdict = VERDICT_BAD, .where = EVENT_BMS_NFAULT_CHECK, .mv = summary };
    } else if (bc->sense_timing && lowMs >= BMS_SDC_SENSE_MS) {
        *j = (judged_t){
            .verdict = VERDICT_BAD,
            .where = EVENT_BMS_SDC_SENSE,
            .mv = (uint16_t)((lowMs < UINT16_MAX) ? lowMs : UINT16_MAX),
        };
    } else if (bc->feedback_bad == 0u && registerRead && bc->nfault_check_bad == 0u &&
               !bc->sense_timing) {
        j->verdict = VERDICT_CLEAN;
    } else {
        j->verdict = VERDICT_UNKNOWN;
    }
}

static uint8_t next_state(const bms_control_t *bc, const bms_control_in_t *in, bool charge_mode)
{
    const uint8_t run = charge_mode ? BMS_STATE_CHARGE : BMS_STATE_DRIVE;

    if (bc->config.held) {
        return BMS_STATE_FAULT_HELD;
    }
    if (!in->startup_ok) {
        return BMS_STATE_STARTUP; /* a blocked start-up never leaves it */
    }
    if (bc->active != 0u) {
        return BMS_STATE_FAULT;
    }
    /* BMS_OK rises only while the last open-wire check that judged passed
       (spec #36: one has passed before it first rises). */
    switch (bc->state) {
    case BMS_STATE_STARTUP:
        return (bc->all_clean >= BMS_POWERUP_SWEEPS && bc->ow_passed) ? run : BMS_STATE_STARTUP;
    case BMS_STATE_FAULT:
        return (bc->all_clean >= BMS_RECOVER_SWEEPS && bc->ow_passed) ? run : BMS_STATE_FAULT;
    default:
        return run;
    }
}

static void add_balance_events(bms_control_out_t *out, const bms_balance_out_t *bo)
{
    for (size_t i = 0; i < bo->event_count && out->event_count < BMS_CONTROL_EVENTS_MAX; i++) {
        bms_event_t *ev = &out->events[out->event_count++];
        ev->code = bo->events[i].code;
        memcpy(ev->data, bo->events[i].data, EVENT_DATA_SIZE);
    }
}

static void add_charge_events(bms_control_out_t *out, const bms_charge_events_t *ce)
{
    for (size_t i = 0; i < ce->count && out->event_count < BMS_CONTROL_EVENTS_MAX; i++) {
        bms_event_t *ev = &out->events[out->event_count++];
        ev->code = ce->events[i].code;
        memcpy(ev->data, ce->events[i].data, EVENT_DATA_SIZE);
    }
}

static void add_event(bms_control_out_t *out, const event_bms_fault_t *e)
{
    if (out->event_count < BMS_CONTROL_EVENTS_MAX) {
        bms_event_t *ev = &out->events[out->event_count++];
        ev->code = EVENT_BMS_FAULT;
        event_data_bms_fault(ev->data, e);
    }
}

static uint8_t has(uint16_t bits, bms_fault_t f)
{
    return (bits & bit(f)) != 0u ? 1u : 0u;
}

/* The next frame of this step's. Never more than BMS_TX_FRAMES_MAX: the
   send table puts three on a step (bms_tx.h). */
static bms_tx_frame_t *next_frame(bms_control_out_t *out)
{
    return &out->frames[out->frame_count++];
}

_Static_assert(BMS_TX_FRAMES_MAX >= 3u,
               "BMS_Status, BMS_Power and one slower frame fit on a step");

static void build_status(bms_control_t *bc, const bms_control_in_t *in, bms_control_out_t *out)
{
    const uint16_t a = out->active;
    const uint16_t s = out->seen;
    const struct can_control_bms_status_t st = {
        .state = out->state,
        .over_voltage_active = has(a, BMS_FAULT_OVER_VOLTAGE),
        .under_voltage_active = has(a, BMS_FAULT_UNDER_VOLTAGE),
        .over_temp_active = has(a, BMS_FAULT_OVER_TEMP),
        .under_temp_active = has(a, BMS_FAULT_UNDER_TEMP),
        .measurement_active = has(a, BMS_FAULT_MEASUREMENT),
        .open_wire_active = has(a, BMS_FAULT_OPEN_WIRE),
        .cell_monitor_active = has(a, BMS_FAULT_CELL_MONITOR),
        .internal_active = has(a, BMS_FAULT_INTERNAL),
        .charger_disobeys_active = has(a, BMS_FAULT_CHARGER_DISOBEYS),
        .overcurrent_active = has(a, BMS_FAULT_OVERCURRENT),
        .over_voltage_seen = has(s, BMS_FAULT_OVER_VOLTAGE),
        .under_voltage_seen = has(s, BMS_FAULT_UNDER_VOLTAGE),
        .over_temp_seen = has(s, BMS_FAULT_OVER_TEMP),
        .under_temp_seen = has(s, BMS_FAULT_UNDER_TEMP),
        .measurement_seen = has(s, BMS_FAULT_MEASUREMENT),
        .open_wire_seen = has(s, BMS_FAULT_OPEN_WIRE),
        .cell_monitor_seen = has(s, BMS_FAULT_CELL_MONITOR),
        .internal_seen = has(s, BMS_FAULT_INTERNAL),
        .charger_disobeys_seen = has(s, BMS_FAULT_CHARGER_DISOBEYS),
        .overcurrent_seen = has(s, BMS_FAULT_OVERCURRENT),
        .bms_ok = out->bms_ok ? 1u : 0u,
        .charge_complete = (out->charge == BMS_CHARGE_COMPLETE) ? 1u : 0u,
        .balancing_stopped = bms_balance_stopped(&bc->balance) ? 1u : 0u,
        .low_voltage_limit = out->under_low_voltage_limit ? 1u : 0u,
        .shutdown_circuit_closed = in->sdc_closed ? 1u : 0u,
        .parameters_defaulted = bc->config.params_defaulted ? 1u : 0u,
        .not_dual_bank = bc->config.not_dual_bank ? 1u : 0u,
        .last_reset_not_power_on = bc->config.last_reset_not_power_on ? 1u : 0u,
        .bus_off_latched = in->bus_off_latched ? 1u : 0u,
        .error_passive = in->error_passive ? 1u : 0u,
        .receive_overflow = in->rx_overflowed ? 1u : 0u,
    };

    bms_tx_build_status(&st, &bc->e2e, next_frame(out));
}

void bms_control_step(bms_control_t *bc, const bms_control_in_t *in, bms_control_out_t *out)
{
    judged_t j[BMS_FAULT_COUNT];
    uint16_t raised = 0u;
    bool allClean = true;

    memset(out, 0, sizeof *out);

    if (!bc->started || in->swept) {
        bc->last_sweep_ms = in->now_ms;
    }
    bc->started = true;

    /* The charger first (bms_charge.h): Charge mode, and whether it
       disobeys. Its Events go after the fault Events. */
    bms_charge_events_t chargeEvents = { .count = 0u };
    const bms_charge_heard_t heard =
        bms_charge_listen(&bc->charge, in->now_ms, in->charger, &chargeEvents);

    const bool startedUp = bc->state != BMS_STATE_STARTUP;
    judge(in, heard.charge_mode, startedUp, j);
    judge_charger(&heard, &j[BMS_FAULT_CHARGER_DISOBEYS]);
    judge_open_wire(bc, in, &j[BMS_FAULT_OPEN_WIRE]);
    judge_cell_monitor(in, startedUp, &j[BMS_FAULT_CELL_MONITOR]);
    judge_internal(bc, in, &j[BMS_FAULT_INTERNAL]);

    /* A chain that has brought back no Sweep for 2 s in STARTUP: the
       measurement fault at once (spec #36). */
    if (bc->state == BMS_STATE_STARTUP && !in->swept &&
        (uint32_t)(in->now_ms - bc->last_sweep_ms) >= BMS_CHAIN_SILENT_MS) {
        j[BMS_FAULT_MEASUREMENT] = (judged_t){ .verdict = VERDICT_BAD };
        if (bc->bad[BMS_FAULT_MEASUREMENT] < BMS_MEAS_SWEEPS) {
            bc->bad[BMS_FAULT_MEASUREMENT] = BMS_MEAS_SWEEPS - 1u;
        }
    }

    for (bms_fault_t f = 0; f < BMS_FAULT_COUNT; f++) {
        switch (j[f].verdict) {
        case VERDICT_BAD:
            count_up(&bc->bad[f]);
            bc->clean[f] = 0u;
            break;
        case VERDICT_CLEAN:
            bc->bad[f] = 0u;
            count_up(&bc->clean[f]);
            break;
        default: /* unknown: the bad count holds; it is not clean */
            bc->clean[f] = 0u;
            break;
        }
        /* The open-wire check judges once a second, not each Sweep: it
           gates BMS_OK in next_state() instead. */
        if (f != BMS_FAULT_OPEN_WIRE) {
            allClean = allClean && j[f].verdict == VERDICT_CLEAN;
        }

        if (raiseAt[f] != 0u && (bc->active & bit(f)) == 0u && bc->bad[f] >= raiseAt[f]) {
            bc->active |= bit(f);
            bc->seen |= bit(f);
            raised |= bit(f);
        } else if ((bc->active & bit(f)) != 0u && bc->clean[f] >= BMS_RECOVER_SWEEPS) {
            bc->active &= (uint16_t)~bit(f);
        }
    }
    if (allClean) {
        count_up(&bc->all_clean);
    } else {
        bc->all_clean = 0u;
    }

    /* The Held fault (bms_control_held()): the internal fault from the
       first step on, never clearing. */
    if (bc->config.held) {
        if ((bc->seen & bit(BMS_FAULT_INTERNAL)) == 0u) {
            raised |= bit(BMS_FAULT_INTERNAL);
            j[BMS_FAULT_INTERNAL] = (judged_t){ .verdict = VERDICT_BAD,
                                                .where = EVENT_BMS_HELD };
        }
        bc->active |= bit(BMS_FAULT_INTERNAL);
        bc->seen |= bit(BMS_FAULT_INTERNAL);
    }

    bc->state = next_state(bc, in, heard.charge_mode);

    const uint16_t devicesRead = in->swept ? in->sweep->devices_read : 0u;
    for (bms_fault_t f = 0; f < BMS_FAULT_COUNT; f++) {
        if ((raised & bit(f)) != 0u) {
            const event_bms_fault_t e = {
                .fault = (uint8_t)f,
                .state = bc->state,
                .where = j[f].where,
                .index = j[f].index,
                .reading_mv = j[f].mv,
                .devices_read = (f == BMS_FAULT_OPEN_WIRE) ? bc->ow_failed.devices_read
                                                           : devicesRead,
            };
            add_event(out, &e);
        }
    }

    out->state = bc->state;
    out->bms_ok = bc->state == BMS_STATE_DRIVE || bc->state == BMS_STATE_CHARGE;
    out->active = bc->active;
    out->seen = bc->seen;
    bc->commanded = out->bms_ok;

    /* Derating (bms_derate.h), on this Sweep's lowest cell and hottest
       sensor; 0 A while BMS_OK is low. A step with no Sweep, or one that
       read no cell or no sensor, holds it. */
    if (in->swept) {
        bms_tx_summarise(in->sweep, &bc->summary);
    }
    const bool swept = in->swept && bc->summary.cells != 0u && bc->summary.temps != 0u;
    out->discharge_limit_da = bms_derate_step(&bc->derate, in->now_ms, swept,
                                              bc->summary.min_mv, bc->summary.max_dc,
                                              in->pack_current, out->bms_ok);
    out->under_low_voltage_limit = bms_derate_under_low_voltage_limit(&bc->derate);
    out->summary = bc->summary;

    /* Balancing (bms_balance.h), BMS_OK as written; the charge state is
       the last step's. Its second is the open-wire check's: the check may
       go on a step with a Sweep, once a second, unless balancing holds it. */
    const bool checkDue =
        in->swept &&
        (!bc->ow_asked || (uint32_t)(in->now_ms - bc->ow_asked_ms) >= BMS_OPEN_WIRE_PERIOD_MS);
    const bms_balance_in_t balanceIn = {
        .now_ms = in->now_ms,
        .charge_mode = heard.charge_mode,
        .charge_complete = bms_charge_state(&bc->charge) == BMS_CHARGE_COMPLETE,
        .state_charge = bc->state == BMS_STATE_CHARGE,
        .sdc_closed = in->sdc_closed,
        .bms_ok = out->bms_ok,
        .fault_active = bc->active != 0u,
        .charger_heard = in->charger->state == MAILBOX_VALID,
        .swept = in->swept,
        .sweep = in->sweep,
        .check_due = checkDue,
        .checked = bc->ow_asked,
        .last_check_ms = bc->ow_asked_ms,
        .ran = in->balance_ran,
        .result = &in->balance_result,
    };
    const bms_balance_out_t *balance = &bc->balance_out;
    bms_balance_step(&bc->balance, &balanceIn, &bc->balance_out);
    out->balance_command = balance->command;
    out->balance_state = balance->state;
    out->balance_reason = balance->reason;
    out->cell_spread_mv = balance->spread_mv;
    memcpy(out->bleeding, balance->on, sizeof out->bleeding);

    /* The charge (bms_charge.h), on the last settled Sweep only (ADR
       0020), BMS_OK as written. */
    if (in->swept && balance->settled) {
        bc->settled = bc->summary;
    }
    const bms_charge_in_t chargeIn = {
        .now_ms = in->now_ms,
        .swept = in->swept && balance->settled && bc->settled.cells != 0u &&
                 bc->settled.temps != 0u,
        .summary = &bc->settled,
        .bms_ok = out->bms_ok,
        .fault_active = bc->active != 0u,
        .sdc_closed = in->sdc_closed,
    };
    out->charge_limit_da = bms_charge_step(&bc->charge, &chargeIn, &chargeEvents);
    out->charge = bms_charge_state(&bc->charge);
    add_charge_events(out, &chargeEvents);
    add_balance_events(out, balance);

    /* The open-wire check: after a Sweep, never in place of one, never
       with a balancing command; the first at once, then once a second
       (spec #36). */
    if (checkDue && !balance->hold_check) {
        bc->ow_asked = true;
        bc->ow_asked_ms = in->now_ms;
        out->open_wire_due = true;
    }
    /* The heartbeat's 5 Hz (issue 47): a BMS fault, or a blocked start-up. */
    out->fault_active = bc->active != 0u || bc->state == BMS_STATE_FAULT ||
                        bc->state == BMS_STATE_FAULT_HELD || !in->startup_ok || !in->can_running;

    /* The send table (bms_tx.h). Each frame is built only when it goes, so
       its E2E counter counts frames sent. */
    const bms_tx_values_t values = {
        .summary = &bc->summary,
        .pack_current = in->pack_current,
        .discharge_limit_da = out->discharge_limit_da,
        .charge_limit_da = out->charge_limit_da,
    };
    if (bms_tx_status_due(in->step_count)) {
        build_status(bc, in, out);
    }
    if (bms_tx_power_due(in->step_count)) {
        bms_tx_build_power(&values, &bc->e2e, next_frame(out));
    }
    const bms_tx_slow_t slow = bms_tx_slow_due(in->step_count);
    if (bc->state == BMS_STATE_CHARGE && bms_tx_charger_request_due(in->step_count)) {
        /* In place of 0x202, in CHARGE only (bms_tx.h). */
        struct can_control_chg_request_t req;
        bms_charge_request(&bc->charge, in->now_ms, &req);
        bms_tx_build_charger_request(&req, next_frame(out));
    } else if (heard.charge_mode && bms_tx_balancing_due(in->step_count)) {
        /* In place of 0x202, in Charge mode only (bms_tx.h). */
        bms_tx_build_balancing(out->balance_state, out->balance_reason, out->cell_spread_mv,
                               next_frame(out));
    } else if (slow != BMS_TX_SLOW_NONE) {
        bms_tx_build_slow(slow, &values, &bc->e2e, next_frame(out));
    }
}

const char *bms_control_state_name(uint8_t state)
{
    switch (state) {
    case BMS_STATE_STARTUP:    return "STARTUP";
    case BMS_STATE_DRIVE:      return "DRIVE";
    case BMS_STATE_CHARGE:     return "CHARGE";
    case BMS_STATE_FAULT:      return "FAULT";
    case BMS_STATE_FAULT_HELD: return "FAULT_HELD";
    default:                   return "?";
    }
}
