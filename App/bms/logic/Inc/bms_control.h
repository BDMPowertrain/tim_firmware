/*
 * The BMS's Control step as one function (issue 63; spec #36, "BMS image";
 * ADR 0007): from the step's Sweep and the time, the state, BMS_OK, the
 * BMS_Status frame and the Events. bms_step.c takes the Sweep, calls
 * bms_control_step() and writes what comes out, in the step order sweep,
 * judge, write BMS_OK, send, publish. Nothing here touches hardware.
 *
 * States (BMS_Status State values, can/control.dbc):
 *   STARTUP     BMS_OK low. To DRIVE (CHARGE in Charge mode) once every
 *               task and queue exists, the chain is awake (its post-wake
 *               faults cleared), an open-wire check has passed and
 *               BMS_POWERUP_SWEEPS Sweeps in a row were clean. To FAULT
 *               when a BMS fault is raised, and when the chain has brought
 *               back no Sweep for BMS_CHAIN_SILENT_MS: the measurement
 *               fault. A start-up that is blocked (a task or queue
 *               missing) never leaves STARTUP.
 *   DRIVE       BMS_OK high. To FAULT on any BMS fault.
 *   CHARGE      as DRIVE, in Charge mode (in.charge_mode, #67).
 *   FAULT       BMS_OK low. Ends by itself after BMS_RECOVER_SWEEPS clean
 *               Sweeps in a row (1 s) with no fault active and the last
 *               open-wire check passed: every value has been in range for
 *               1 s (ADR 0007).
 *   FAULT_HELD  the Held fault (config.held, from bms_control_held()):
 *               BMS_OK low from the first step until the next Power-on
 *               reset, with the internal fault active (ADR 0007, 0011).
 * Firmware latches nothing else: the shutdown board's latch keeps the
 * Tractive System off until a person resets it (ADR 0007, EV.8.2.3).
 *
 * BMS faults, each judged every Sweep and raised after its count of bad
 * Sweeps in a row (spec #36, the BMS fault table; EV.8.3.4, EV.8.4.2,
 * EV.8.5.2):
 *   over-voltage       a cell at BMS_OV_MV or above, BMS_OV_SWEEPS
 *   under-voltage      a cell at BMS_UV_MV or below (the raw loaded
 *                      voltage, no sag compensation), BMS_UV_SWEEPS
 *   over-temperature   a sensor reading at BMS_OT_MV or below (58 C),
 *                      BMS_TEMP_SWEEPS
 *   under-temperature  a sensor reading at BMS_UT_MV or above (2 C),
 *                      BMS_TEMP_SWEEPS, in Charge mode only
 *   measurement        a device not read, a cell reading 0 mV, or a sensor
 *                      reading outside CELL_TEMP_MV_MIN..MAX, or, after
 *                      STARTUP, a step with no Sweep (chain comms loss),
 *                      BMS_MEAS_SWEEPS
 * A reading that is missing or invalid is judged by the measurement fault
 * alone: for the others that Sweep neither counts as bad nor as clean, so
 * a device that answers only now and then cannot hold a cell's fault off.
 * A step with no Sweep is the same for them.
 * A fault's active bit clears after BMS_RECOVER_SWEEPS Sweeps clean of it;
 * its seen bit only at a Power-on reset. Each raised fault is an EVENT_BMS_FAULT Event
 * naming the fault and the cell or sensor (event_data_bms_fault()).
 * The second layers (issue 64):
 *   open sense wire    BMS_OPEN_WIRE_CHECKS failed open-wire checks in a
 *                      row (EV.8.3.4 b: a blown or tripped voltage sense
 *                      fuse reads as an open wire; 2.5 s budget). The step
 *                      asks for the check (out.open_wire_due) on a step
 *                      that took a Sweep: the first, then one at least
 *                      BMS_OPEN_WIRE_PERIOD_MS after the last, and hands
 *                      its result in on the next step. A check that could
 *                      not judge neither counts nor ends a run. BMS_OK
 *                      first rises only once a check has passed, and only
 *                      while the last one did.
 *   cell monitor       NFAULT low (after STARTUP, or with a Sweep), or the
 *                      bridge's FAULT_SUMMARY non-zero (a device reporting
 *                      its own fault), BMS_CELL_MONITOR_SWEEPS. These are
 *                      the stack's comparators behind the firmware's
 *                      judgement (bq_chain.h); NFAULT has already taken
 *                      BMS_OK low by hardware (ADR 0008).
 *   internal           any of three self-checks (EV.8.3.4 e):
 *                      - the BMS_OK read-back differs from the level last
 *                        commanded AND the NFAULT read-back,
 *                        BMS_FEEDBACK_SWEEPS in a row;
 *                      - the NFAULT read-back disagrees with the bridge's
 *                        FAULT_SUMMARY (a dead bridge reads "no fault"
 *                        through NFAULT's pull-up), BMS_FEEDBACK_SWEEPS
 *                        Sweeps in a row, judged only with a Sweep;
 *                      - the BMS_OK read-back low for BMS_SDC_SENSE_MS with
 *                        the Shutdown Circuit sense energised throughout.
 *                      And (issue 65) the timing fault: three Overruns
 *                      within one second (overrun_watch.h), raised on the
 *                      step after the third. The Overrun Events say which
 *                      steps ran long and in which phase.
 * Charger disobeys is #67; overcurrent is reserved and never set.
 *
 * The minimum cell under the Low-voltage limit (the voltage taper's 100%
 * point, a parameter; bms_derate.h) at the latest Sweep is a BMS_Status
 * bit and never a BMS fault.
 *
 * Derating (issue 66, ADR 0018): the Discharge limit, from the lowest cell
 * and the hottest sensor of each Sweep and the pack current, by
 * bms_derate.h, and 0 A while BMS_OK is low. The charge limit is 0 A out
 * of Charge mode (#67 adds the rest). A step with no Sweep, or a Sweep
 * with no valid cell or no valid sensor, holds the limit: the measurement
 * fault takes BMS_OK, and so the limit, to zero within 150 ms.
 *
 * The frames (bms_tx.h: the send table): BMS_Status and BMS_Power every
 * step, one slower frame a step. BMS_Power, BMS_Limits and the summaries
 * carry the last Sweep's readings, over the cells and sensors it read
 * validly; pack voltage is the sum of the cells read. Pack current goes as
 * 0 A while it is invalid (issue 29 decides how to mark it).
 *
 * The BMS makes no decision from any received frame (spec #36, user story
 * 26): the Control bus only fills BMS_Status bits, and a bus failure never
 * takes BMS_OK low (ADR 0005).
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_CONTROL_H
#define BMS_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bms_derate.h"
#include "bms_tx.h"
#include "bq_chain.h"
#include "bq_sweep.h"
#include "can_control.h"
#include "e2e_p11.h"
#include "event_log.h"
#include "reset_cause.h"

#define BMS_STATE_STARTUP    CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE
#define BMS_STATE_DRIVE      CAN_CONTROL_BMS_STATUS_STATE_DRIVE_CHOICE
#define BMS_STATE_CHARGE     CAN_CONTROL_BMS_STATUS_STATE_CHARGE_CHOICE
#define BMS_STATE_FAULT      CAN_CONTROL_BMS_STATUS_STATE_FAULT_CHOICE
#define BMS_STATE_FAULT_HELD CAN_CONTROL_BMS_STATUS_STATE_FAULT_HELD_CHOICE

/* The thresholds carry the measurement-accuracy margin (spec #36; ADR
   0015: constants, never Parameters). */
#define BMS_OV_MV           4195u /* cell at or above */
#define BMS_UV_MV           2505u /* cell at or below */
#define BMS_OT_MV           1526u /* sensor reading at or below: 58 C */
#define BMS_UT_MV           2146u /* sensor reading at or above: 2 C */

/* Bad Sweeps in a row to raise the fault, at 20 ms each. */
#define BMS_OV_SWEEPS      3u  /* 100 ms budget */
#define BMS_UV_SWEEPS      10u /* 250 ms */
#define BMS_TEMP_SWEEPS    25u /* 550 ms */
#define BMS_MEAS_SWEEPS    5u  /* 150 ms */

#define BMS_CELL_MONITOR_SWEEPS 1u /* NFAULT has already acted */
#define BMS_FEEDBACK_SWEEPS     5u /* the read-back checks */
#define BMS_SDC_SENSE_MS        500u
#define BMS_OPEN_WIRE_CHECKS    2u    /* failed in a row: 2.5 s budget */
#define BMS_OPEN_WIRE_PERIOD_MS 1000u

#define BMS_RECOVER_SWEEPS 50u   /* 1 s clean ends a fault */
#define BMS_POWERUP_SWEEPS 5u    /* clean Sweeps before BMS_OK first rises */
#define BMS_CHAIN_SILENT_MS 2000u /* no Sweep in STARTUP for this long: FAULT */

/* BMS_Status fault bits, in its order: bit (1 << bms_fault_t) of
   out.active and out.seen. */
typedef enum {
    BMS_FAULT_OVER_VOLTAGE     = 0,
    BMS_FAULT_UNDER_VOLTAGE    = 1,
    BMS_FAULT_OVER_TEMP        = 2,
    BMS_FAULT_UNDER_TEMP       = 3,
    BMS_FAULT_MEASUREMENT      = 4,
    BMS_FAULT_OPEN_WIRE        = 5,
    BMS_FAULT_CELL_MONITOR     = 6,
    BMS_FAULT_INTERNAL         = 7,
    BMS_FAULT_CHARGER_DISOBEYS = 8,
    BMS_FAULT_OVERCURRENT      = 9, /* reserved, never set (issue 29) */
    BMS_FAULT_COUNT
} bms_fault_t;

/* Fixed from start-up to the next reset. */
typedef struct {
    bool held;                    /* the Held fault: bms_control_held() */
    bool params_defaulted;
    bool not_dual_bank;
    bool last_reset_not_power_on;
    bms_derate_config_t derate;   /* from the BMS parameters */
} bms_control_config_t;


typedef struct {
    uint32_t          now_ms;
    uint32_t          step_count;  /* 0 on the first step: the send table's */
    bool              startup_ok;  /* every task and queue exists */
    bool              can_running; /* CAN1 started: only the heartbeat shows it */
    bool              swept;       /* this step took a Sweep */
    const bq_sweep_t *sweep;       /* it, when swept */
    bool              charge_mode; /* Charge mode (#67) */
    bool              sdc_closed;  /* the Shutdown Circuit sense reads energised */
    bool              nfault_ok;   /* FB.NFAULT reads high: no fault */
    bool              bms_ok_fb;   /* FB.BMS_OK reads high */
    bool              timing_fault;  /* the last step was the third Overrun
                                        within one second (control_step.h) */
    bool              open_wire_ran; /* the last step ran the open-wire check */
    bq_open_wire_t    open_wire;     /* its result, when it did */
    bool              bus_off_latched;
    bool              error_passive;
    bool              rx_overflowed;
    bms_current_t     pack_current;  /* positive discharging, with its validity */
} bms_control_in_t;

typedef struct {
    uint16_t code;
    uint8_t  data[EVENT_DATA_SIZE];
} bms_event_t;

/* One per fault, all raised in one step at most. */
#define BMS_CONTROL_EVENTS_MAX ((size_t)BMS_FAULT_COUNT)

typedef struct {
    uint8_t  state;
    bool     bms_ok;            /* the level to write on BMS_OK: true = high */
    bool     fault_active;      /* for the heartbeat LED */
    uint16_t active;            /* bit (1 << bms_fault_t) */
    uint16_t seen;
    bool     under_low_voltage_limit;
    /* Run the open-wire check after this step's work, and hand its result
       in on the next step. */
    bool     open_wire_due;

    /* The frames due on this step, built (bms_tx.h says which), to send in
       this order. */
    size_t         frame_count;
    bms_tx_frame_t frames[BMS_TX_FRAMES_MAX];

    uint16_t discharge_limit_da; /* the Discharge limit sent, 0.1 A */
    bms_pack_summary_t summary;  /* the last Sweep's, for the snapshot */

    size_t      event_count;
    bms_event_t events[BMS_CONTROL_EVENTS_MAX];
} bms_control_out_t;

/* The step's own state. bms_control_init() it; touch nothing inside. */
typedef struct {
    bms_control_config_t config;
    uint8_t      state;
    bool         started;          /* a step has run */
    uint32_t     last_sweep_ms;    /* the last Sweep, or the first step */
    uint16_t     bad[BMS_FAULT_COUNT];   /* bad Sweeps in a row */
    uint16_t     clean[BMS_FAULT_COUNT]; /* Sweeps clean of it in a row */
    uint16_t     all_clean;        /* Sweeps clean of every fault in a row */
    uint16_t     active;
    uint16_t     seen;
    bms_tx_e2e_t e2e;
    bms_derate_t derate;
    bms_pack_summary_t summary;    /* the last Sweep's */
    /* The self-checks. */
    bool         commanded;        /* BMS_OK as last written; low from reset */
    uint16_t     feedback_bad;     /* read-back wrong, Sweeps in a row */
    uint16_t     nfault_check_bad; /* NFAULT against the register, Sweeps in a row */
    bool         sense_timing;     /* BMS_OK low with the sense energised ... */
    uint32_t     sense_since_ms;   /* ... since */
    /* The open-wire check. */
    bool         ow_asked;         /* one has been asked for */
    uint32_t     ow_asked_ms;      /* when the last was */
    uint8_t      ow_fails;         /* failed in a row */
    bool         ow_passed;        /* the last that judged passed */
    bq_open_wire_t ow_failed;      /* the last that failed */
} bms_control_t;

/*
 * The Held fault (ADR 0007, 0011): true after any reset that was not a
 * Power-on reset (the BOR flag set, a brown-out included), so the step
 * holds BMS_OK low until the next one. A reset with the core debug enable
 * bit set (debugger: DHCSR.C_DEBUGEN as start-up found it) is a Power-on
 * reset, in every build: the bit cannot be set without a probe on the SWD
 * header and stays set until a power cycle. A reset that left no flag is
 * not shown to be a Power-on reset, so it holds too.
 */
bool bms_control_held(reset_cause_t cause, bool debugger);

void bms_control_init(bms_control_t *bc, const bms_control_config_t *config);

void bms_control_step(bms_control_t *bc, const bms_control_in_t *in, bms_control_out_t *out);

/* "STARTUP", "DRIVE", ... "?" for a value that is not a state. */
const char *bms_control_state_name(uint8_t state);

#endif /* BMS_CONTROL_H */
