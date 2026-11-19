/*
 * Balancing (issue 68; ADR 0020; spec #36, "BMS image"): which cells
 * bleed, when, and the chain commands that make it so. bms_control.h calls
 * bms_balance_step() once a step, after the state and BMS_OK, and runs the
 * command it returns after the step's work (bq_chain.h), never in the step
 * that runs the open-wire check. No balancing failure is a BMS fault.
 *
 * Firmware decides; the cell monitors' auto mode and voltage stop are not
 * used. Each second, lined up with the open-wire check, is:
 *
 *   the decision  the step whose Sweep may take the open-wire check
 *                 (in.check_due) and was settled: taken with every FET
 *                 off for at least BMS_BALANCE_PAUSE_MS. That Sweep alone
 *                 chooses the cells (and feeds the charge taper and the
 *                 end-of-charge test: out.settled). The check
 *                 runs after it in the same step.
 *   the set       the next step writes the chosen set (BQ_BALANCE_SET),
 *                 each bleeding cell armed with the chip's 10 s timer, so
 *                 a hung or reset MCU leaves no cell bleeding for longer.
 *                 Writing it every second re-arms it.
 *   bleeding      about 920 ms.
 *   the pause     BMS_BALANCE_PERIOD_MS - BMS_BALANCE_PAUSE_MS after the
 *                 last check every FET goes off (BQ_BALANCE_STOP), which
 *                 also reads each board's die temperature for the next
 *                 decision. The next check's Sweep is the settled one
 *                 (out.settled). The pause's stop goes every second while
 *                 balancing may run, bleeding or not, so each decision has
 *                 fresh die temperatures.
 * The 20 ms protective Sweeps go on throughout; bms_control.h judges every
 * one of them for the BMS faults.
 *
 * Choosing, on the settled Sweep, once the Balancing gate has latched:
 *   - nothing unless the Sweep read every cell, and nothing but the
 *     low-cell stop unless it read every sensor validly;
 *   - a session starts at a Cell spread over BMS_BALANCE_START_SPREAD_MV
 *     and is done at BMS_BALANCE_DONE_SPREAD_MV or less (DONE); a later
 *     spread over the start begins another;
 *   - in a session, every cell more than BMS_BALANCE_BLEED_ABOVE_MV above
 *     the lowest cell bleeds, alternating odd and even cells within a
 *     board each second (a board with none of this second's parity bleeds
 *     the other), so at most 4 of its 8 bleed and no two neighbours are
 *     on;
 *   - a board whose die temperature was not read at the pause, or that is
 *     hot (BMS_BALANCE_BOARD_HOT_DC, until BMS_BALANCE_BOARD_COOL_DC),
 *     does not bleed: PAUSED, BOARD_HOT when no board can;
 *   - every board pauses while the hottest sensor is at
 *     BMS_BALANCE_CELLS_HOT_DC (until under BMS_BALANCE_CELLS_COOL_DC):
 *     PAUSED, CELLS_HOT.
 *
 * The Balancing gate is BMS_BALANCE_GATE_MV under the Charge target. It
 * latches for the charge (until a power cycle) on the first settled Sweep
 * whose highest cell reaches it; before that the state is WAITING_FOR_GATE.
 *
 * The interlock (EV.8.3.3): the BMS state CHARGE, the Shutdown Circuit
 * sense closed, BMS_OK high with no BMS fault and the charger heard (its
 * mailbox VALID, the 5 s charger timeout). Any of them false sends the
 * stop in the same step (within about 40 ms of the sense opening, the
 * sense read at the step's start and the stop at its end) and PAUSES:
 * SENSE_OPEN, BMS_FAULT or CHARGER_SILENT, in that order. Balancing
 * resumes at the first decision after all have held for
 * BMS_BALANCE_RESTART_MS. A silent charger is a pause like the others.
 * Out of the CHARGE state (STARTUP, FAULT, FAULT_HELD) the interlock is
 * false, and outside Charge mode (DRIVE) the state is OFF: in every such
 * state no FET is commanded on, and one that may be on gets the stop.
 *
 * Before the first bleed of the charge, once, the FET check
 * (bq_chain.h): the odd FETs, then the even, each on for
 * BMS_BALANCE_FET_SETTLE_MS, compared for BMS_BALANCE_FET_COMPARE_MS, then
 * read. It runs after a decision and is done before the next pause; the
 * open-wire check waits for it. A FET that fails stops balancing; a check
 * that could not be read whole is run again at the next decision.
 *
 * STOPPED, until a power cycle, with an Event and the BMS_Status bit
 * (bms_balance_stopped()):
 *   LOW_CELL          the lowest cell of a settled Sweep more than
 *                     BMS_BALANCE_LOW_CELL_MV under the Charge target
 *   TIME_LIMIT        BMS_BALANCE_TIME_LIMIT_MS after Charge complete,
 *                     counted through pauses, not DONE: no convergence.
 *                     A DONE balancing then stays DONE, with an Event
 *                     (DONE, TIME_LIMIT) for the end; no session starts.
 *   FET_CHECK_FAILED  the FET check
 *   CONFIG_REJECTED   a device refused a set (BAL_STAT[INVALID_CBCONF])
 * None is a BMS fault: the cell voltage faults remain the protection.
 *
 * Each change of state or reason is an EVENT_BALANCING Event. Every
 * number here is a compile-time constant (ADR 0020): tuned by reflash,
 * not stored.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_BALANCE_H
#define BMS_BALANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bq_chain.h"
#include "bq_sweep.h"
#include "can_control.h"
#include "event_log.h"

#define BMS_BALANCE_GATE_MV          100u /* the gate: under the Charge target */
#define BMS_BALANCE_START_SPREAD_MV  10u  /* a session starts over it */
#define BMS_BALANCE_BLEED_ABOVE_MV   5u   /* a cell more than this above the lowest bleeds */
#define BMS_BALANCE_DONE_SPREAD_MV   5u   /* a session is done at or under it */
#define BMS_BALANCE_LOW_CELL_MV      200u /* stop: the lowest this far under the target */
#define BMS_BALANCE_TIME_LIMIT_MS    (12u * 60u * 60u * 1000u) /* after Charge complete */
#define BMS_BALANCE_RESTART_MS       3000u /* the interlock held, to resume */
#define BMS_BALANCE_PERIOD_MS        1000u /* the decision: the open-wire check's */
/* The pause: the settled Sweep is the first one this long after the
   step that sent the stop. The stop goes at the end of that step, after
   its Sweep and work, so every FET has been off for this less that step's
   run time (at least 45 ms at the 15 ms plan budget). The tap-wire error
   goes with the current; the cell's own relaxation at under 0.2 A is
   about 1 mV. To be confirmed on the bench (ADR 0020). */
#define BMS_BALANCE_PAUSE_MS         60u
#define BMS_BALANCE_BOARD_HOT_DC     950  /* die temperature: pause the board */
#define BMS_BALANCE_BOARD_COOL_DC    850  /* resume it at or under */
#define BMS_BALANCE_CELLS_HOT_DC     450  /* hottest sensor: pause every board */
/* Resume under: 2 C under the pause, so a reading on the line does not
   pause and resume every second. */
#define BMS_BALANCE_CELLS_COOL_DC    430
/* The FET check (SLUSE81F 7.3.6.4.6.3): the AUX ADC takes every cell once
   before the comparison (4.3 ms each to settle, 8 cells), then the
   comparison over every cell. To be confirmed on the bench. */
#define BMS_BALANCE_FET_SETTLE_MS    60u
#define BMS_BALANCE_FET_COMPARE_MS   100u

#define BMS_BALANCE_ODD_CELLS  0x55u /* cells 1, 3, 5, 7 */
#define BMS_BALANCE_EVEN_CELLS 0xAAu /* cells 2, 4, 6, 8 */

#define BMS_BALANCE_OFF        CAN_CONTROL_BMS_BALANCING_STATE_OFF_CHOICE
#define BMS_BALANCE_WAITING    CAN_CONTROL_BMS_BALANCING_STATE_WAITING_FOR_GATE_CHOICE
#define BMS_BALANCE_BALANCING  CAN_CONTROL_BMS_BALANCING_STATE_BALANCING_CHOICE
#define BMS_BALANCE_PAUSED     CAN_CONTROL_BMS_BALANCING_STATE_PAUSED_CHOICE
#define BMS_BALANCE_DONE       CAN_CONTROL_BMS_BALANCING_STATE_DONE_CHOICE
#define BMS_BALANCE_STOPPED    CAN_CONTROL_BMS_BALANCING_STATE_STOPPED_CHOICE

#define BMS_BALANCE_NONE             CAN_CONTROL_BMS_BALANCING_REASON_NONE_CHOICE
#define BMS_BALANCE_SENSE_OPEN       CAN_CONTROL_BMS_BALANCING_REASON_SENSE_OPEN_CHOICE
#define BMS_BALANCE_BMS_FAULT        CAN_CONTROL_BMS_BALANCING_REASON_BMS_FAULT_CHOICE
#define BMS_BALANCE_CHARGER_SILENT   CAN_CONTROL_BMS_BALANCING_REASON_CHARGER_SILENT_CHOICE
#define BMS_BALANCE_BOARD_HOT        CAN_CONTROL_BMS_BALANCING_REASON_BOARD_HOT_CHOICE
#define BMS_BALANCE_CELLS_HOT        CAN_CONTROL_BMS_BALANCING_REASON_CELLS_HOT_CHOICE
#define BMS_BALANCE_LOW_CELL         CAN_CONTROL_BMS_BALANCING_REASON_LOW_CELL_CHOICE
#define BMS_BALANCE_TIME_LIMIT       CAN_CONTROL_BMS_BALANCING_REASON_TIME_LIMIT_CHOICE
#define BMS_BALANCE_FET_CHECK_FAILED CAN_CONTROL_BMS_BALANCING_REASON_FET_CHECK_FAILED_CHOICE
#define BMS_BALANCE_CONFIG_REJECTED  CAN_CONTROL_BMS_BALANCING_REASON_CONFIG_REJECTED_CHOICE

/* What bms_balance_step() needs, as the step has judged it. */
typedef struct {
    uint32_t now_ms;
    bool     charge_mode;     /* bms_charge.h */
    bool     charge_complete; /* Charge complete has begun */
    bool     state_charge;    /* the BMS state is CHARGE */
    bool     sdc_closed;      /* the Shutdown Circuit sense reads closed */
    bool     bms_ok;          /* the level written on BMS_OK this step */
    bool     fault_active;    /* a BMS fault is active */
    bool     charger_heard;   /* the charger's mailbox VALID */
    bool     swept;           /* this step took a Sweep: sweep */
    const bq_sweep_t *sweep;
    /* This step's Sweep may take the open-wire check (once a second). */
    bool     check_due;
    bool     checked;         /* a check has been asked for ... */
    uint32_t last_check_ms;   /* ... last at */
    /* The command bms_balance_step() returned on the last step, carried
       out: ran, false if it could not be planned (the chain not running),
       and its result. */
    bool     ran;
    const bq_balance_result_t *result;
} bms_balance_in_t;

typedef struct {
    uint16_t code;
    uint8_t  data[EVENT_DATA_SIZE];
} bms_balance_event_t;

/* A state change a step at most, and one more for a stop found while
   applying the last command's result. */
#define BMS_BALANCE_EVENTS_MAX 2u

typedef struct {
    /* This step's Sweep was settled: taken with every FET off for at least
       BMS_BALANCE_PAUSE_MS. Only a settled Sweep may feed the charge taper
       and the end-of-charge test (ADR 0020); while no cell bleeds, every
       Sweep is. */
    bool     settled;
    /* The chain command to run after this step's work (BQ_BALANCE_NONE:
       nothing), and its result handed in on the next step. */
    bq_balance_cmd_t command;
    /* The open-wire check must wait: a command goes this step, or the FET
       check is under way. */
    bool     hold_check;
    uint8_t  state;     /* BMS_Balancing State */
    uint8_t  reason;    /* BMS_Balancing Reason */
    uint16_t spread_mv; /* the last settled Sweep's Cell spread; 0 before one */
    /* Bit k of on[d - 1]: cell k + 1 of device d commanded on now (the
       DAQ bus's per-cell bleed flags). */
    uint8_t  on[BQ_SWEEP_DEVICES];
    size_t   event_count;
    bms_balance_event_t events[BMS_BALANCE_EVENTS_MAX];
} bms_balance_out_t;

typedef enum {
    BMS_FET_CHECK_IDLE,    /* not running */
    BMS_FET_CHECK_DUE,     /* a decision asked for it: FET_ON next */
    BMS_FET_CHECK_ON,      /* the FETs under test on, since stage_ms */
    BMS_FET_CHECK_COMPARE, /* the comparison started, since stage_ms */
    BMS_FET_CHECK_READ,    /* the results asked for */
} bms_fet_check_stage_t;

/* bms_balance_init() it; touch nothing inside. */
typedef struct {
    uint16_t target_mv;    /* the Charge target */
    uint8_t  state;
    uint8_t  reason;
    bool     gate;         /* latched for the charge */
    bool     session;      /* a session is under way */
    bool     ended;        /* the time limit has run out */
    bool     complete_seen;
    uint32_t complete_ms;
    /* The interlock. */
    bool     allowed_timing;
    uint32_t allowed_since_ms;
    uint8_t  interlock_reason; /* the last condition found false */
    /* The FETs. */
    bool     maybe_on;     /* a set or the FET check may have a FET on */
    bool     stop_due;     /* a FET may be on that must not be: stop first */
    bool     ever_off;     /* a stop has gone through ... */
    uint32_t off_ms;       /* ... on the step at */
    uint8_t  on[BQ_SWEEP_DEVICES];
    bool     paused;       /* the pause's stop has gone for ... */
    uint32_t paused_for_ms;/* ... the check asked at */
    bq_balance_cmd_t sent; /* the command returned on the last step ... */
    uint32_t sent_ms;      /* ... on the step at */
    /* The next set, chosen at the decision. */
    bool     set_due;
    uint8_t  set[BQ_SWEEP_DEVICES];
    bool     odd;          /* this second's parity */
    /* The pause's die temperatures. */
    uint16_t die_read;
    int16_t  die_dc[BQ_SWEEP_DEVICES];
    uint16_t board_hot;
    bool     cells_hot;
    /* The FET check. */
    bool     fet_checked;
    bms_fet_check_stage_t fet_stage;
    uint32_t fet_stage_ms;
    uint8_t  fet_cells;    /* the cells under test */
    /* The last settled Sweep that read every cell. */
    bool     extremes;
    uint16_t spread_mv;
    uint16_t max_mv;
    uint16_t min_mv;
    uint8_t  max_cell;     /* 0 to 127 */
    uint8_t  min_cell;
} bms_balance_t;

void bms_balance_init(bms_balance_t *b, uint16_t target_mv);

void bms_balance_step(bms_balance_t *b, const bms_balance_in_t *in, bms_balance_out_t *out);

/* Balancing STOPPED, for the BMS_Status bit. */
bool bms_balance_stopped(const bms_balance_t *b);

/* "OFF", "WAITING_FOR_GATE", ...; "?" otherwise. */
const char *bms_balance_state_name(uint8_t state);
/* "NONE", "SENSE_OPEN", ...; "?" otherwise. */
const char *bms_balance_reason_name(uint8_t reason);

#endif /* BMS_BALANCE_H */
