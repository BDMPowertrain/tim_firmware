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
   here. */
static const uint16_t raiseAt[BMS_FAULT_COUNT] = {
    [BMS_FAULT_OVER_VOLTAGE]  = BMS_OV_SWEEPS,
    [BMS_FAULT_UNDER_VOLTAGE] = BMS_UV_SWEEPS,
    [BMS_FAULT_OVER_TEMP]     = BMS_TEMP_SWEEPS,
    [BMS_FAULT_UNDER_TEMP]    = BMS_TEMP_SWEEPS,
    [BMS_FAULT_MEASUREMENT]   = BMS_MEAS_SWEEPS,
};

void bms_control_init(bms_control_t *bc, const bms_control_config_t *config)
{
    memset(bc, 0, sizeof *bc);
    bc->config = *config;
    bc->state = BMS_STATE_STARTUP;
}

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
static void judge(const bms_control_in_t *in, bool started_up, judged_t j[BMS_FAULT_COUNT],
                  bool *low_voltage)
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
            in->charge_mode ? VERDICT_UNKNOWN : VERDICT_CLEAN;
        j[BMS_FAULT_MEASUREMENT].verdict = started_up ? VERDICT_BAD : VERDICT_UNKNOWN;
        return;
    }

    const bq_sweep_t *s = in->sweep;
    judged_t *meas = &j[BMS_FAULT_MEASUREMENT];
    bool lv = false;
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
            lv = lv || v < BMS_LOW_VOLTAGE_MV;
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
    if (!in->charge_mode) {
        j[BMS_FAULT_UNDER_TEMP].verdict = VERDICT_CLEAN;
    }
    *low_voltage = lv;
}

static uint8_t next_state(const bms_control_t *bc, const bms_control_in_t *in)
{
    const uint8_t run = in->charge_mode ? BMS_STATE_CHARGE : BMS_STATE_DRIVE;

    if (bc->config.held) {
        return BMS_STATE_FAULT_HELD;
    }
    if (!in->startup_ok) {
        return BMS_STATE_STARTUP; /* a blocked start-up never leaves it */
    }
    if (bc->active != 0u) {
        return BMS_STATE_FAULT;
    }
    switch (bc->state) {
    case BMS_STATE_STARTUP:
        return (bc->all_clean >= BMS_POWERUP_SWEEPS) ? run : BMS_STATE_STARTUP;
    case BMS_STATE_FAULT:
        return (bc->all_clean >= BMS_RECOVER_SWEEPS) ? run : BMS_STATE_FAULT;
    default:
        return run;
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
        .low_voltage_limit = out->low_voltage_limit ? 1u : 0u,
        .shutdown_circuit_closed = in->sdc_closed ? 1u : 0u,
        .parameters_defaulted = bc->config.params_defaulted ? 1u : 0u,
        .not_dual_bank = bc->config.not_dual_bank ? 1u : 0u,
        .last_reset_not_power_on = bc->config.last_reset_not_power_on ? 1u : 0u,
        .bus_off_latched = in->bus_off_latched ? 1u : 0u,
        .error_passive = in->error_passive ? 1u : 0u,
        .receive_overflow = in->rx_overflowed ? 1u : 0u,
    };

    bms_tx_build_status(&st, &bc->status_tx, out->status);
    out->send_status = true;
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

    bool lv = bc->low_voltage_limit; /* held over a step with no Sweep */
    judge(in, bc->state != BMS_STATE_STARTUP, j, &lv);
    bc->low_voltage_limit = lv;

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
        allClean = allClean && j[f].verdict == VERDICT_CLEAN;

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

    /* The Held fault: the internal fault from the first step on, never
       clearing (#65 decides when). */
    if (bc->config.held) {
        if ((bc->seen & bit(BMS_FAULT_INTERNAL)) == 0u) {
            raised |= bit(BMS_FAULT_INTERNAL);
            j[BMS_FAULT_INTERNAL] = (judged_t){ .verdict = VERDICT_BAD };
        }
        bc->active |= bit(BMS_FAULT_INTERNAL);
        bc->seen |= bit(BMS_FAULT_INTERNAL);
    }

    bc->state = next_state(bc, in);

    const uint16_t devicesRead = in->swept ? in->sweep->devices_read : 0u;
    for (bms_fault_t f = 0; f < BMS_FAULT_COUNT; f++) {
        if ((raised & bit(f)) != 0u) {
            const event_bms_fault_t e = {
                .fault = (uint8_t)f,
                .state = bc->state,
                .where = j[f].where,
                .index = j[f].index,
                .reading_mv = j[f].mv,
                .devices_read = devicesRead,
            };
            add_event(out, &e);
        }
    }

    out->state = bc->state;
    out->bms_ok = bc->state == BMS_STATE_DRIVE || bc->state == BMS_STATE_CHARGE;
    out->active = bc->active;
    out->seen = bc->seen;
    out->low_voltage_limit = bc->low_voltage_limit;
    /* The heartbeat's 5 Hz (issue 47): a BMS fault, or a blocked start-up. */
    out->fault_active = bc->active != 0u || bc->state == BMS_STATE_FAULT ||
                        bc->state == BMS_STATE_FAULT_HELD || !in->startup_ok || !in->can_running;
    /* Built only when it goes, so its E2E counter counts frames sent. */
    if (bms_tx_status_due(in->step_count)) {
        build_status(bc, in, out);
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
