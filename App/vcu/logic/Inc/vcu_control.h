/*
 * The VCU's Control step as one function (issue 56; spec #36, "VCU image";
 * ADR 0006): from the step's inputs and the time, the state, the safety
 * outputs, the frames to send and the Events. vcu_step.c reads the pins, the
 * ADC windows and the mailboxes into a vcu_control_in_t, calls
 * vcu_control_step() and writes what comes out. Nothing here touches
 * hardware.
 *
 * States (VCU_Status State values, can/control.dbc). There is no FAULT
 * state: faults are bits that cut torque or force a transition down.
 *   STARTUP         to GLV_ON once every task and queue exists, the
 *                   Parameters are loaded or defaulted, CAN1 runs and every
 *                   analog input has a sample. Nothing else enters GLV_ON
 *                   from STARTUP, and nothing returns to STARTUP, so a reset
 *                   while driving lands in GLV_ON.
 *   GLV_ON          to TS_ACTIVE when the Shutdown Circuit input reads
 *                   closed, every BMS and inverter mailbox is VALID,
 *                   BMS_Status reports no fault and the inverter's DC bus is
 *                   at least 60 V and at least 90% of the pack voltage in
 *                   BMS_Limits.
 *   TS_ACTIVE       to GLV_ON when a VALID INV_Voltage reads below 60 V
 *                   (EV.5.4.5); to READY_TO_DRIVE by the Start procedure
 *                   (below). A silent inverter leaves the state as it is:
 *                   its Substitute is no news of a discharged bus, and the
 *                   Start procedure needs every mailbox VALID anyway.
 *   READY_TO_DRIVE  Leaving Ready to Drive, to TS_ACTIVE at once, on the
 *                   Shutdown Circuit input open, a BMS-reported fault, an
 *                   inverter-reported fault, the bus-off latch, or the bus
 *                   below 80% of the pack voltage. A fault is "reported"
 *                   only by a VALID mailbox, and the bus voltage is judged
 *                   only with both its mailboxes VALID: a mailbox that is
 *                   not VALID is a Torque cut (ADR 0006, issue 58), and its
 *                   Substitute must not also leave Ready to Drive.
 *
 * Start procedure (EV.10.6): in TS_ACTIVE, with nothing in start_blockers
 * (VCU_START_... below), a press of the start button while Brake engaged
 * was already true on the step before. A press is the first step the button
 * reads pressed after reading released for VCU_START_RELEASE_MS, so a
 * button already held when the brake goes down, or one that bounces as it
 * is let go, never counts; neither does one held since reset.
 *
 * Brake engaged: either brake sensor reads OK and above the brake_engaged
 * Parameter, with VCU_BRAKE_HYSTERESIS counts of hysteresis per sensor. The
 * brake light follows it in every state, and is also on whenever a brake
 * sensor is not OK and while the brake-sensor Torque cut lasts.
 *
 * The torque path (issue 57), in the step's order: the pedal sensors and
 * the plausibility timers; Brake engaged and the brake-and-pedal check;
 * the state transitions; the driver request; the limiters (an empty slot
 * until issue 60); the gates; the send.
 *   Driver request: from the lower Pedal travel, 0 below 5%, then linear
 *   from 0 at 5% to the max_torque Parameter at 100%. Never negative.
 *   Gates: zero torque outside READY_TO_DRIVE, on the first 0x0C0 after
 *   entry, and while any Torque cut is active.
 *
 * Torque cuts (ADR 0006, vcu_cut_t). Each keeps Enable=1 with zero torque
 * and ends only once its cause is gone and the
 * higher Pedal travel is below 5%, so torque never returns under a pressed
 * pedal. Each start and end is an EVENT_TORQUE_CUT Event. A cut only zeroes
 * torque in Ready to Drive (the Glossary's Torque cut), but its timer and
 * latch run in every state, so a cause already present when Ready to
 * Drive is entered cuts from the first step, and the Start procedure is
 * refused while one is active (VCU_START_TORQUE_CUT).
 *   APPS implausible  more than 10 points apart, or either APPS not OK
 *                     (out of range, its supply out of window, no sample),
 *                     for VCU_CUT_PERSIST_MS (T.4.2.4, T.4.2.5, T.4.2.10)
 *   brake sensor      either brake sensor not OK for VCU_CUT_PERSIST_MS
 *                     (T.4.3.3, T.4.3.4)
 *   brake and pedal   Brake engaged with the higher travel above 25%, at
 *                     once (EV.4.7)
 *   timing fault      three Overruns within a second (in.timing_fault)
 *
 * Inverter command (0x0C0): every step in every state after STARTUP, the
 * rolling counter one more each frame, direction forward, the torque limit
 * bytes the maximum-torque Parameter. Enable=1 only in READY_TO_DRIVE, and
 * the first frame after entry carries zero torque.
 *
 * Ready to Drive sound (EV.10.7): on for VCU_SOUND_MS from entry into
 * READY_TO_DRIVE, off at once if it is left.
 *
 * Fault LED: on in STARTUP and for VCU_LAMP_CHECK_MS after reset release
 * (config.reset_ms); solid from Leaving Ready to Drive until the Start
 * procedure is done again or the state is GLV_ON; otherwise blinking every
 * VCU_FAULT_BLINK_MS while a Torque cut lasts. The fault-active test point
 * and fault_active (the heartbeat's 5 Hz) are on for a Torque cut or a
 * restart needed, in the same step that zeroes the command.
 *
 * VCU_Status: every second step from the first, with the state, the bits
 * this step knows, the flags and the torque in this step's 0x0C0.
 *
 * Every state transition is an EVENT_VCU_STATE Event with its cause.
 *
 * Calibrated (ADR 0015, 0016; vcu_calibration.h), judged once at init from
 * the config. Not calibrated, or the Parameters defaulted, the VCU still
 * reaches GLV_ON (the shell is needed to calibrate) but both APPS read
 * NOT_CALIBRATED, so the APPS Torque cut is on and the Start procedure
 * refused (VCU_START_NOT_CALIBRATED, VCU_START_PARAMS_DEFAULTED); the
 * fault LED is on, and VCU_Status carries NotCalibrated or
 * ParametersDefaulted. Not calibrated is an EVENT_NOT_CALIBRATED Event on
 * the step that leaves STARTUP (parameters.c writes the defaulted one).
 *
 * Pedal check (ADR 0016; vcu_pedal_check.h): due unless the Event log holds
 * a pass for this image and these Parameters (in.pedal_check_record) or
 * one passed since reset; due refuses the Start procedure
 * (VCU_START_PEDAL_CHECK_DUE) and sets PedalCheckDue. The sequence runs
 * only while due, in GLV_ON (the Tractive System off), Calibrated and with
 * the Parameters loaded; anything else starts it over. A pass is an
 * EVENT_PEDAL_CHECK Event with the build ID, the Parameters' save counter
 * and the calibration's fingerprint; a fail under way is an
 * EVENT_PEDAL_CHECK_FAILED Event.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_CONTROL_H
#define VCU_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_control.h"
#include "control_rx.h"
#include "e2e_p11.h"
#include "event_log.h"
#include "mailbox.h"
#include "vcu_calibration.h"
#include "vcu_pedal_check.h"
#include "vcu_sensors.h"

#define VCU_STATE_STARTUP        CAN_CONTROL_VCU_STATUS_STATE_STARTUP_CHOICE
#define VCU_STATE_GLV_ON         CAN_CONTROL_VCU_STATUS_STATE_GLV_ON_CHOICE
#define VCU_STATE_TS_ACTIVE      CAN_CONTROL_VCU_STATUS_STATE_TS_ACTIVE_CHOICE
#define VCU_STATE_READY_TO_DRIVE CAN_CONTROL_VCU_STATUS_STATE_READY_TO_DRIVE_CHOICE

#define VCU_SOUND_MS          2000u /* EV.10.7 allows 1-3 s */
#define VCU_LAMP_CHECK_MS     3000u
#define VCU_START_RELEASE_MS  30u   /* start button debounce */
#define VCU_BRAKE_HYSTERESIS  25u   /* ADC counts, about 34 mV at the sensor */
#define VCU_TRAVEL_RELEASED   50u   /* Pedal travel below 5%, in tenths */
#define VCU_TS_MIN_DV         600   /* 60 V in 0.1 V: a discharged bus, EV.5.4.5 */
#define VCU_TS_UP_PERCENT     90u   /* bus to pack voltage to be TS Active */
#define VCU_TS_LOST_PERCENT   80u   /* below it, Leaving Ready to Drive */

/* The frames the VCU reads, one mailbox each, in this order (the
   receivers in can/control.dbc). vcu_rx_frames[] gives each its row in
   control_rx_config. */
typedef enum {
    VCU_RX_BMS_STATUS,
    VCU_RX_BMS_LIMITS,
    VCU_RX_BMS_POWER,
    VCU_RX_INV_MOTOR_POSITION,
    VCU_RX_INV_VOLTAGE,
    VCU_RX_INV_INTERNAL_STATES,
    VCU_RX_INV_FAULT_CODES,
    VCU_RX_INV_TORQUE_CAPABILITY,
    VCU_RX_EM_MEASUREMENT,
    VCU_RX_EM_STATUS,
    VCU_RX_IMD_INFO_GENERAL,
    VCU_RX_COUNT
} vcu_rx_t;

extern const control_rx_t vcu_rx_frames[VCU_RX_COUNT];

/* What refuses the Start procedure now: start_blockers bits. */
#define VCU_START_NOT_TS_ACTIVE    0x0001u /* the state is not TS_ACTIVE */
#define VCU_START_BRAKE            0x0002u /* Brake engaged is false */
#define VCU_START_PEDAL            0x0004u /* higher Pedal travel 5% or more */
#define VCU_START_PEDAL_SENSOR     0x0008u /* an APPS reading is not OK */
#define VCU_START_BRAKE_SENSOR     0x0010u /* a brake sensor reading is not OK */
#define VCU_START_PARAMS_DEFAULTED 0x0020u
#define VCU_START_MAILBOX          0x0040u /* a BMS or inverter mailbox is not VALID */
#define VCU_START_BMS_FAULT        0x0080u /* BMS_Status reports a fault */
#define VCU_START_INVERTER_FAULT   0x0100u /* INV_FaultCodes reports a fault */
#define VCU_START_SDC_OPEN         0x0200u /* the Shutdown Circuit input reads open */
#define VCU_START_BUS_OFF          0x0400u /* the bus-off latch is set */
#define VCU_START_BUS_VOLTAGE      0x0800u /* bus below 60 V or below 90% of pack */
#define VCU_START_TIMING_FAULT     0x1000u /* the timing-fault Torque cut is active */
#define VCU_START_TORQUE_CUT       0x2000u /* any Torque cut is active */
#define VCU_START_NOT_CALIBRATED   0x4000u /* the calibration is not Calibrated */
#define VCU_START_PEDAL_CHECK_DUE  0x8000u /* no Pedal check for this image and Parameters */

/* What the search of the Event log found (in.pedal_check_record). */
#define VCU_PEDAL_CHECK_UNKNOWN 0u /* not searched yet: due */
#define VCU_PEDAL_CHECK_FOUND   1u /* a pass for this image and these Parameters */
#define VCU_PEDAL_CHECK_ABSENT  2u /* none: due */

/* An EVENT_VCU_STATE Event's cause (event_codes.h). */
typedef enum {
    VCU_CAUSE_STARTUP_DONE    = 1, /* STARTUP to GLV_ON */
    VCU_CAUSE_TS_UP           = 2, /* GLV_ON to TS_ACTIVE */
    VCU_CAUSE_BUS_DISCHARGED  = 3, /* TS_ACTIVE to GLV_ON: bus below 60 V */
    VCU_CAUSE_START_PROCEDURE = 4, /* TS_ACTIVE to READY_TO_DRIVE */
    /* Leaving Ready to Drive, the first that applies; the Event also holds
       a mask of all that applied, bit (cause - VCU_CAUSE_SDC_OPEN). */
    VCU_CAUSE_SDC_OPEN        = 5,
    VCU_CAUSE_BMS_FAULT       = 6,
    VCU_CAUSE_INVERTER_FAULT  = 7,
    VCU_CAUSE_BUS_OFF_LATCHED = 8,
    VCU_CAUSE_BUS_VOLTAGE_LOW = 9,
} vcu_cause_t;

/* A Torque cut's cause (ADR 0006): bit (1 << cause) of cuts, and byte 0 of
   an EVENT_TORQUE_CUT Event. Each keeps Enable=1 with zero torque. */
typedef enum {
    VCU_CUT_APPS         = 1, /* APPS implausible for VCU_CUT_PERSIST_MS */
    VCU_CUT_BRAKE_SENSOR = 2, /* a brake sensor failed for VCU_CUT_PERSIST_MS */
    VCU_CUT_BRAKE_PEDAL  = 3, /* Brake engaged with the higher travel above 25% */
    VCU_CUT_TIMING       = 4, /* three Overruns within a second */
    VCU_CUT_COUNT             /* one past the last; arrays by vcu_cut_t leave [0] unused */
} vcu_cut_t;

#define VCU_CUT_PERSIST_MS   80u  /* T.4.2.5, T.4.3.3 allow 100 ms */
#define VCU_APPS_AGREE       100u /* T.4.2.4: 10 points, in tenths */
#define VCU_TRAVEL_BRAKE_CUT 250u /* EV.4.7: 25%, in tenths */
#define VCU_FAULT_BLINK_MS   250u /* fault LED half-period in a Torque cut: 2 Hz */

/* Fixed from start-up to the next reset (ADR 0015). */
typedef struct {
    vcu_sensor_cal_t cal;
    uint16_t brake_engaged;  /* the brake_engaged Parameter, counts above zero */
    uint16_t cal_revision;   /* the cal_revision Parameter */
    bool     cal_present;    /* every calibration Parameter came from a valid copy */
    uint16_t max_torque;     /* the max_torque Parameter, 0.1 N.m */
    uint32_t build_id;       /* the image's, for a Pedal check pass */
    bool     build_dirty;    /* built from a tree with uncommitted changes */
    uint32_t param_save_counter; /* the loaded copy's, 0 if none */
    bool     params_defaulted;
    bool     not_dual_bank;
    bool     last_reset_not_power_on;
    uint32_t reset_ms;       /* the clock at reset release, for the lamp check */
} vcu_control_config_t;

typedef struct {
    uint32_t         now_ms;
    uint32_t         step_count;  /* 0 on the first step */
    bool             startup_ok;  /* every task and queue exists, Parameters loaded or defaulted */
    bool             can_running; /* CAN1 started */
    vcu_sensor_raw_t raw;         /* averaged ADC counts */
    bool             sdc_closed;  /* the Shutdown Circuit input reads closed */
    bool             start_pressed;
    const mailbox_t *mailboxes;   /* VCU_RX_COUNT, in vcu_rx_t order, updated this step */
    bool             bus_off_latched;
    bool             error_passive;
    bool             rx_overflowed;
    bool             timing_fault; /* control_step_ctx_t.timing_fault */
    uint8_t          pedal_check_record; /* VCU_PEDAL_CHECK_... */
} vcu_control_in_t;

typedef struct {
    uint16_t code;
    uint8_t  data[EVENT_DATA_SIZE];
} vcu_event_t;

/* A state Event and every Torque cut starting or ending in one step. */
#define VCU_CONTROL_EVENTS_MAX 8u

typedef struct {
    uint8_t       state;
    vcu_sensors_t sensors;
    bool          samples_ready;  /* every analog input has a sample */
    bool          brake_engaged;
    uint16_t      start_blockers; /* VCU_START_... bits; 0: a press would start */
    bool          startup_blocked;
    bool          fault_active;   /* for the heartbeat LED */
    int16_t       torque_request; /* the driver's, from the lower Pedal travel, 0.1 N.m */
    int16_t       torque;         /* in this step's 0x0C0 after the gates, 0.1 N.m */
    uint16_t      cuts;           /* the Torque cuts active: bit (1 << vcu_cut_t) */
    vcu_cal_result_t  calibration;    /* VCU_CAL_OK: Calibrated */
    bool              pedal_check_due;
    vcu_pedal_check_t pedal_check;    /* how far the sequence has got */

    /* Pin levels, true = on. */
    bool          brake_light;
    bool          sound;
    bool          fault_led;
    bool          test_point;

    bool          send_command;   /* command holds an INV_Command (0x0C0) */
    uint8_t       command[CAN_CONTROL_INV_COMMAND_LENGTH];
    bool          send_status;    /* status holds a VCU_Status (0x090) */
    uint8_t       status[CAN_CONTROL_VCU_STATUS_LENGTH];

    size_t        event_count;
    vcu_event_t   events[VCU_CONTROL_EVENTS_MAX];
} vcu_control_out_t;

/* One Torque cut's timer and latch. */
typedef struct {
    bool     pending;   /* the cause holds now */
    uint32_t since_ms;  /* since when */
    bool     active;    /* the cut is on */
} vcu_cut_timer_t;

/* The step's own state. vcu_control_init() it; touch nothing inside. */
typedef struct {
    vcu_cut_timer_t cut[VCU_CUT_COUNT]; /* by vcu_cut_t; [0] unused */
    bool         cut_was;        /* a Torque cut was active on the last step */
    uint32_t     blink_from_ms;  /* the first step of the Torque cuts now active */
    bool         restart_needed; /* Ready to Drive was left for a fault */
    vcu_control_config_t config;
    uint8_t      state;
    uint32_t     entered_ms;     /* when the state was entered */
    bool         lamp_check_done;
    bool         sensor_engaged[VCU_BRAKE_COUNT];
    bool         brake_was_engaged;
    bool         button_released;  /* read released on the last step */
    uint32_t     button_released_ms; /* since when */
    uint8_t      rolling_counter;  /* the next 0x0C0's */
    bool         enable_sent;      /* the last 0x0C0 carried Enable = 1 */
    e2e_p11_tx_t status_tx;
    vcu_cal_result_t  calibration;    /* judged at init */
    uint32_t          cal_fingerprint; /* vcu_cal_fingerprint() of the config's */
    vcu_pedal_check_t pedal_check;
    bool         pedal_check_passed; /* since reset */
} vcu_control_t;

void vcu_control_init(vcu_control_t *vc, const vcu_control_config_t *config);

void vcu_control_step(vcu_control_t *vc, const vcu_control_in_t *in,
                      vcu_control_out_t *out);

/* "STARTUP", "GLV_ON", ... "?" for a value that is not a state. */
const char *vcu_control_state_name(uint8_t state);

/* The Parameter save hook's rule (ADR 0015): NULL allows the save, which
   is only in GLV_ON, so nothing is written while the car can move. running
   is false before the first step. */
const char *vcu_control_save_refusal(bool running, uint8_t state);

/* The bench commands' refusal hook's rule (`hang`, `crash`, a `term`
   override; ADR 0002 amendment): refused in READY_TO_DRIVE, where any of
   them could cut the drive with no warning to the driver. */
const char *vcu_control_bench_refusal(bool running, uint8_t state);

#endif /* VCU_CONTROL_H */
