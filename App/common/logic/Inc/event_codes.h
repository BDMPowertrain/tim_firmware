/*
 * Event codes (spec #36): every code is assigned here and nowhere else,
 * with the layout of its 16 data bytes, little-endian, unused bytes zero.
 * A code is never reused for another meaning once an image has written
 * it: a stored Event outlives the image that wrote it.
 *
 * Codes for Events whose source is not built yet (charger,
 * balancing and the rest of the spec's list)
 * are added here when their source is.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef EVENT_CODES_H
#define EVENT_CODES_H

#include <stdbool.h>
#include <stdint.h>

#include "event_log.h"
#include "overrun_watch.h"
#include "params.h"
#include "reset_record.h"

typedef enum {
    /* The Event log itself (the writer in App/common/Src/events.c). */
    EVENT_BOOT             = 1,  /* event_data_boot(); first of each start */
    EVENT_BOOT_CRASH       = 2,  /* event_data_crash(): the Crash record, after the boot Event */
    EVENT_BOOT_BREADCRUMB  = 3,  /* event_data_breadcrumb(): the Breadcrumb, after the boot Event */
    EVENT_STORAGE_REPAIRED = 4,  /* event_data_repair() */
    EVENT_EVENTS_DROPPED   = 5,  /* 0 N (u32) */
    EVENT_NOT_DUAL_BANK    = 6,  /* no data; never stored, as nothing is */

    /* Parameters (ADR 0015). */
    EVENT_PARAMS_DEFAULTED   = 16, /* event_data_params_defaulted() */
    EVENT_PARAM_OUT_OF_RANGE = 17, /* event_data_param_out_of_range() */
    EVENT_PARAM_SAVE         = 18, /* 0 params_save_result_t (u8), 4 save counter (u32) */

    /* The Control step (ADR 0010, 0013). */
    EVENT_OVERRUN = 32, /* event_data_overrun() */

    /* The Control bus (ADR 0004, 0005), all event_data_bus(). */
    EVENT_BUS_OFF           = 48, /* a bus-off seen by the step */
    EVENT_ERROR_PASSIVE     = 49, /* the controller became error-passive */
    EVENT_BUS_OFF_LATCHED   = 50, /* the third quick bus-off: retry once per second */
    EVENT_BUS_OFF_UNLATCHED = 51, /* clean long enough: the latch cleared */
    EVENT_MAILBOX_TIMEOUT   = 52, /* event_data_mailbox_timeout() */

    /* The VCU's states (ADR 0006, vcu_control.h). */
    EVENT_VCU_STATE  = 64, /* event_data_vcu_state() */
    EVENT_TORQUE_CUT = 65, /* event_data_torque_cut(): one cause starting or ending */
    EVENT_INVERTER_FAULT       = 66, /* event_data_inverter_fault(): new fault bits reported */
    EVENT_INVERTER_FAULT_CLEAR = 67, /* event_data_fault_clear(): a 0x0C1 sent, or its result */
    EVENT_DISCHARGE_SLOW       = 68, /* event_data_discharge_slow() */
    EVENT_EM_VIOLATION         = 69, /* event_data_em_violation(): the Violation bit set */

    /* Calibration and the Pedal check (ADR 0016, vcu_control.h). */
    EVENT_PEDAL_CHECK        = 70, /* event_data_pedal_check(): a pass */
    EVENT_PEDAL_CHECK_FAILED = 71, /* event_data_pedal_check_failed() */
    EVENT_NOT_CALIBRATED     = 72, /* event_data_not_calibrated() */

    /* The Data log (ADR 0021, data_log.h). */
    EVENT_DATA_LOG_CARD = 80, /* event_data_data_log_card(): no card, or a card error */

    /* The BMS's faults (ADR 0007, bms_control.h). */
    EVENT_BMS_FAULT = 96, /* event_data_bms_fault(): a BMS fault raised */
} event_code_t;

/* "boot", "overrun" and so on, for the shell; "?" for a code not here. */
const char *event_code_name(uint16_t code);

/* Boot flags (byte 2 of the boot Event). */
#define EVENT_BOOT_DIRTY      0x01u /* built from a tree with uncommitted changes */
#define EVENT_BOOT_CRASH_NEXT 0x02u /* an EVENT_BOOT_CRASH follows */
#define EVENT_BOOT_CRUMB_NEXT 0x04u /* an EVENT_BOOT_BREADCRUMB follows */

typedef struct {
    uint8_t       image;    /* PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS */
    uint32_t      build_id; /* the commit the image was built from */
    bool          dirty;
    const reset_report_t *reset;
} event_boot_t;

/* 0 image (u8), 1 reset cause (u8), 2 flags (u8), 4 build ID (u32),
   8 resets not power-on (u32). The boot Event's 16 bytes cannot also hold
   the Crash record and the Breadcrumb, so each follows as its own Event:
   the Crash record when there is one,
   and the Breadcrumb when a step ran, neither after a Power-on reset. */
void event_data_boot(uint8_t data[EVENT_DATA_SIZE], const event_boot_t *boot);

/* 0 crash cause (u8), 4 address (u32), 8 task name (8 chars, NUL-padded). */
void event_data_crash(uint8_t data[EVENT_DATA_SIZE], const crash_record_t *crash);

/* 0 step count (u32), 4 Overruns (u32), 8 phase (u8). */
void event_data_breadcrumb(uint8_t data[EVENT_DATA_SIZE], const breadcrumb_t *crumb);

/* 0 page (u8), 1 Events kept (u8), 2 header bad (u8), 3 erased (u8),
   4 pages repaired at this open (u32). */
void event_data_repair(uint8_t data[EVENT_DATA_SIZE], const event_log_repair_t *repair,
                       uint32_t repairs);

/* 0 N (u32). */
void event_data_dropped(uint8_t data[EVENT_DATA_SIZE], uint32_t n);

/* 0 params_source_t (u8), 1 copy A status (u8), 2 copy B status (u8),
   4 missing (u16), 6 out of range (u16), 8 unknown (u16). */
void event_data_params_defaulted(uint8_t data[EVENT_DATA_SIZE], const params_t *p);

/* 0 ID (u16), 4 stored value (i32), 8 default now in force (i32). */
void event_data_param_out_of_range(uint8_t data[EVENT_DATA_SIZE], uint16_t id, int32_t stored,
                                   int32_t def);

/* 0 params_save_result_t (u8), 4 save counter (u32). */
void event_data_param_save(uint8_t data[EVENT_DATA_SIZE], uint8_t result, uint32_t counter);

/* 0 step (us, u32), 4 slowest phase (us, u32), 8 slowest phase index (u8),
   9 the image's extra work ran in the step (u8, 0 or 1). */
void event_data_overrun(uint8_t data[EVENT_DATA_SIZE], const overrun_event_t *overrun);

typedef struct {
    uint32_t bus_offs;       /* since power-on */
    uint32_t error_passives; /* since power-on */
    uint8_t  tec;            /* transmit error count, now */
    uint8_t  rec;            /* receive error count, now */
    bool     latched;
} event_bus_t;

/* 0 bus-offs (u32), 4 error-passives (u32), 8 TEC (u8), 9 REC (u8),
   10 latched (u8). */
void event_data_bus(uint8_t data[EVENT_DATA_SIZE], const event_bus_t *bus);

/* 0 frame ID (u32), 4 timeouts of this mailbox (u32), 8 row (u8). */
void event_data_mailbox_timeout(uint8_t data[EVENT_DATA_SIZE], uint32_t frame_id,
                                uint32_t timeouts, uint8_t row);

typedef struct {
    uint8_t  from;    /* VCU_Status State values */
    uint8_t  to;
    uint8_t  cause;   /* vcu_cause_t (vcu_control.h) */
    int16_t  bus_dv;  /* inverter DC bus, 0.1 V, as the step read it */
    uint16_t pack_dv; /* BMS_Limits pack voltage, 0.1 V, as the step read it */
    uint16_t causes;  /* Leaving Ready to Drive: every cause that applied */
} event_vcu_state_t;

/* 0 from (u8), 1 to (u8), 2 cause (u8), 4 bus voltage (i16, 0.1 V),
   6 pack voltage (u16, 0.1 V), 8 causes (u16). */
void event_data_vcu_state(uint8_t data[EVENT_DATA_SIZE], const event_vcu_state_t *state);

typedef struct {
    uint8_t  cause;           /* vcu_cut_t (vcu_control.h) */
    bool     started;         /* true: the cut starts; false: it ends */
    uint8_t  state;           /* VCU_Status State value at the time */
    uint16_t travel[2];       /* APPS1, APPS2 Pedal travel, tenths of a percent */
    uint8_t  apps_status[2];  /* vcu_sensor_status_t (vcu_sensors.h) */
    uint8_t  brake_status[2]; /* front, rear */
} event_torque_cut_t;

/* 0 cause (u8), 1 started (u8), 2 state (u8), 4 APPS1 travel (u16),
   6 APPS2 travel (u16), 8 APPS1 status (u8), 9 APPS2 status (u8),
   10 front brake status (u8), 11 rear brake status (u8). */
void event_data_torque_cut(uint8_t data[EVENT_DATA_SIZE], const event_torque_cut_t *cut);

/* 0 POST faults (u32), 4 run faults (u32): INV_FaultCodes as read. */
void event_data_inverter_fault(uint8_t data[EVENT_DATA_SIZE], uint32_t post_faults,
                               uint32_t run_faults);

/* An EVENT_INVERTER_FAULT_CLEAR Event's result. */
typedef enum {
    EVENT_FAULT_CLEAR_SENT    = 0, /* the 0x0C1 went out */
    EVENT_FAULT_CLEAR_CLEARED = 1, /* INV_FaultCodes then read no fault */
    EVENT_FAULT_CLEAR_FAILED  = 2, /* still a fault, or no VALID frame, in time */
} event_fault_clear_result_t;

typedef struct {
    event_fault_clear_result_t result;
    uint8_t  state;       /* VCU_Status State value at the time */
    uint32_t post_faults; /* INV_FaultCodes as last read */
    uint32_t run_faults;
    uint32_t since_ms;    /* since the 0x0C1 went; 0 for EVENT_FAULT_CLEAR_SENT */
} event_fault_clear_t;

/* 0 result (u8), 1 state (u8), 4 POST faults (u32), 8 run faults (u32),
   12 ms since the 0x0C1 (u32). */
void event_data_fault_clear(uint8_t data[EVENT_DATA_SIZE], const event_fault_clear_t *clear);

/* 0 bus voltage (i16, 0.1 V), 4 ms since the Shutdown Circuit input
   opened (u32). */
void event_data_discharge_slow(uint8_t data[EVENT_DATA_SIZE], int16_t bus_dv, uint32_t open_ms);

/* Where a BMS fault was seen (byte 2 of an EVENT_BMS_FAULT Event). */
#define EVENT_BMS_NOWHERE 0u /* no one cell or sensor: the chain brought back nothing */
#define EVENT_BMS_CELL    1u /* a cell voltage; for the open-wire check, a cell
                                under its threshold (reading 0) */
#define EVENT_BMS_SENSOR  2u /* a cell temperature sensor */
/* Issue 64. Index 0 unless said; the reading is not in millivolts. */
#define EVENT_BMS_DEVICE       3u /* a stack device (index 1 to 16) that did
                                     not finish the open-wire check */
#define EVENT_BMS_NFAULT       4u /* NFAULT low; reading: the bridge's
                                     FAULT_SUMMARY, 0 if not read */
#define EVENT_BMS_BRIDGE       5u /* a device reported a fault; reading: the
                                     bridge's FAULT_SUMMARY */
#define EVENT_BMS_FEEDBACK     6u /* the BMS_OK read-back wrong; reading: bit 0
                                     read-back, bit 1 commanded, bit 2 NFAULT */
#define EVENT_BMS_NFAULT_CHECK 7u /* NFAULT and the bridge's FAULT_SUMMARY
                                     disagree; reading: FAULT_SUMMARY */
#define EVENT_BMS_SDC_SENSE    8u /* BMS_OK low with the Shutdown Circuit sense
                                     energised; reading: ms BMS_OK was low */
/* Issue 65. */
#define EVENT_BMS_TIMING       9u /* three Overruns within one second; the
                                     Overrun Events before it say which steps */
#define EVENT_BMS_HELD        10u /* the Held fault; the boot Event says which
                                     reset */

typedef struct {
    uint8_t  fault;        /* bms_fault_t (bms_control.h) */
    uint8_t  state;        /* BMS_Status State value once it is raised */
    uint8_t  where;        /* EVENT_BMS_... */
    uint16_t index;        /* 1 to 128: cell or sensor ((index - 1) % 8) + 1 of
                              device ((index - 1) / 8) + 1; 0 for nowhere */
    uint16_t reading_mv;   /* its reading in the Sweep that raised it (mV for
                              a cell or sensor; see EVENT_BMS_... otherwise) */
    uint16_t devices_read; /* that Sweep's: bit d - 1 set, device d read */
} event_bms_fault_t;

/* 0 fault (u8), 1 state (u8), 2 where (u8), 4 index (u16), 6 reading
   (u16, mV), 8 devices read (u16). */
void event_data_bms_fault(uint8_t data[EVENT_DATA_SIZE], const event_bms_fault_t *fault);

/* 0 VCU state (u8). */
void event_data_em_violation(uint8_t data[EVENT_DATA_SIZE], uint8_t state);

/* 0 build ID (u32), 4 parameter save counter (u32), 8 flags (u8): bit 0,
   a dirty build; 12 the calibration's fingerprint (u32,
   vcu_cal_fingerprint()). */
void event_data_pedal_check(uint8_t data[EVENT_DATA_SIZE], uint32_t build_id,
                            uint32_t save_counter, uint32_t calibration, bool dirty);

/* The running image and the loaded Parameters, for a Pedal check search. */
typedef struct {
    uint32_t build_id;
    uint32_t save_counter; /* the loaded copy's */
    uint32_t calibration;  /* the loaded calibration's fingerprint: the save
                              counter starts again at 1 when no copy is good */
    bool     dirty;        /* the running image is a dirty build */
} event_pedal_check_key_t;

/* ADR 0016: ev is a Pedal check pass for key (an event_pedal_check_key_t),
   in the form event_log_find() takes. A dirty build, or one with no build
   ID (0), never matches, nor does a pass a dirty build wrote: two
   uncommitted images share an ID (cmake/build_id_write.cmake). */
bool event_pedal_check_matches(const event_t *ev, const void *key);

/* 0 vcu_pedal_check_result_t (u8, vcu_pedal_check.h), 1 phase reached
   (u8), 4 APPS1 travel (u16), 6 APPS2 travel (u16). */
void event_data_pedal_check_failed(uint8_t data[EVENT_DATA_SIZE], uint8_t reason, uint8_t phase,
                                   const uint16_t travel[2]);

/* 0 vcu_cal_reason_t (u8, vcu_calibration.h), 1 vcu_cal_sensor_t (u8),
   2 stored calibration revision (u16), 4 the image's (u16). */
void event_data_not_calibrated(uint8_t data[EVENT_DATA_SIZE], uint8_t reason, uint8_t sensor,
                               uint16_t stored_revision, uint16_t image_revision);

typedef struct {
    uint8_t  state;   /* data_log_state_t (data_log_card.h): MISSING or ERROR */
    uint8_t  result;  /* the FatFS result (FRESULT) that failed */
    uint32_t written; /* Records written since power-on */
    uint32_t dropped; /* Records dropped at the rings since power-on */
} event_data_log_card_t;

/* 0 state (u8), 1 FatFS result (u8), 4 Records written (u32),
   8 Records dropped (u32). */
void event_data_data_log_card(uint8_t data[EVENT_DATA_SIZE], const event_data_log_card_t *card);

#endif /* EVENT_CODES_H */
