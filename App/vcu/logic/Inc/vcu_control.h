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
 * brake light follows it in every state. (A failed brake sensor lighting
 * it is the brake-sensor Torque cut's, issue 57.)
 *
 * Inverter command (0x0C0): every step in every state after STARTUP, the
 * rolling counter one more each frame, direction forward, the torque limit
 * bytes the maximum-torque Parameter. Enable=1 only in READY_TO_DRIVE, and
 * the first frame after entry carries zero torque. The torque is always 0
 * until the torque path (issue 57).
 *
 * Ready to Drive sound (EV.10.7): on for VCU_SOUND_MS from entry into
 * READY_TO_DRIVE, off at once if it is left.
 *
 * Fault LED: on in STARTUP and for VCU_LAMP_CHECK_MS after reset release
 * (config.reset_ms), OR-ed with its normal function (issues 57, 58).
 *
 * VCU_Status: every second step from the first, with the state, the bits
 * this step knows, the flags and the torque in this step's 0x0C0.
 *
 * Every state transition is an EVENT_VCU_STATE Event with its cause.
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
#define VCU_START_TIMING_FAULT     0x1000u /* three Overruns within a second */

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

/* Fixed from start-up to the next reset (ADR 0015). */
typedef struct {
    vcu_sensor_cal_t cal;
    uint16_t brake_engaged;  /* the brake_engaged Parameter, counts above zero */
    uint16_t max_torque;     /* the max_torque Parameter, 0.1 N.m */
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
} vcu_control_in_t;

typedef struct {
    uint16_t code;
    uint8_t  data[EVENT_DATA_SIZE];
} vcu_event_t;

#define VCU_CONTROL_EVENTS_MAX 4u

typedef struct {
    uint8_t       state;
    vcu_sensors_t sensors;
    bool          samples_ready;  /* every analog input has a sample */
    bool          brake_engaged;
    uint16_t      start_blockers; /* VCU_START_... bits; 0: a press would start */
    bool          startup_blocked;
    bool          fault_active;   /* for the heartbeat LED */

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

/* The step's own state. vcu_control_init() it; touch nothing inside. */
typedef struct {
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
