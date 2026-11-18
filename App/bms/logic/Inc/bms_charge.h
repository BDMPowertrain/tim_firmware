/*
 * Charge control (issue 67; ADR 0009, 0019; spec #36, "BMS image"): Charge
 * mode, the Charge limit, the charge's start and end, the charger request
 * and the "charger disobeys" judgement. bms_control.h calls it twice a
 * step: bms_charge_listen() before the faults are judged, bms_charge_step()
 * once the state and BMS_OK are known.
 *
 * Charge mode begins on the first good charger status frame (0x18FF50E5)
 * and lasts until a power cycle (ADR 0009). The charger is "heard" while
 * its mailbox is VALID: its 5 s timeout is the charger timeout. The CHG_MODE
 * pin is not read.
 *
 * The Charge limit (ADR 0019) is the full charge limit x the lowest of
 *   voltage taper  100% with the highest cell at or below the Charge
 *                  target - v_band_mv, falling linearly to 0 at the target;
 *   hot taper      100% at or below hot_full_dc, 0 at or above hot_zero_dc;
 *   cold taper     100% at or above cold_full_dc, 0 at or below cold_zero_dc,
 * on the last Sweep's highest cell and its hottest and coldest sensor,
 * moved at the Discharge limit's bounded rates (bms_derate.h; the first
 * Sweep sets it at once). It is 0 A out of Charge mode (in the car), while
 * BMS_OK is low and after Charge complete.
 *
 * The charge, in Charge mode:
 *   WAITING   sends stop. To CHARGING once the charger is heard, no BMS
 *             fault is active, BMS_OK is high, the Shutdown Circuit sense
 *             reads closed and the Charge limit is above 0 A, all for
 *             BMS_CHARGE_START_MS without a break.
 *   CHARGING  asks for the Charge limit. Back to WAITING (stop) the step
 *             any of those but the limit fails: a silent charger, a BMS
 *             fault, BMS_OK low, the sense open. To COMPLETE once the
 *             Charge limit has been under BMS_CHARGE_END_LIMIT_DA with the
 *             highest cell at the target for BMS_CHARGE_END_MS.
 *   COMPLETE  Charge complete: sends stop until a power cycle. The rested
 *             highest and lowest cell go to the Event log
 *             BMS_CHARGE_RESTED_MS after the stop.
 * A silent charger never touches BMS_OK: the state stays CHARGE.
 *
 * The request (CHG_Request, 0x1806E5F4) while CHARGING: voltage 128 x the
 * Charge target (a loose backstop only, ADR 0019), current the Charge
 * limit, control 0; otherwise 0 V, 0 A, control 1. bms_control.h sends it
 * once a second, in the BMS state CHARGE only.
 *
 * Charger disobeys (a BMS fault, ADR 0019): the charger's reported output
 * current more than BMS_CHARGE_DISOBEY_MARGIN_DA over the current last
 * requested for BMS_CHARGE_DISOBEY_MS without a break. A stop requests 0 A,
 * so this is also "over 1 A for 5 s after a stop". A request older than
 * BMS_CHARGE_CHARGER_STOPS_MS counts as 0 A, as the charger stops by
 * itself then (Elcon, Operation Mode 1). A silent charger reads the
 * Substitute, 0 A, and never disobeys. No charger status flag is a BMS
 * fault; each change of them is an Event.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_CHARGE_H
#define BMS_CHARGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bms_tx.h"
#include "can_control.h"
#include "event_log.h"
#include "mailbox.h"

#define BMS_CHARGE_START_MS          3000u  /* the start conditions held */
#define BMS_CHARGE_END_MS            10000u /* the end condition held */
#define BMS_CHARGE_END_LIMIT_DA      5u     /* the end: the limit under 0.5 A */
#define BMS_CHARGE_RESTED_MS         60000u /* the rested cells, after the stop */
#define BMS_CHARGE_DISOBEY_MARGIN_DA 10u    /* 1 A over the request */
#define BMS_CHARGE_DISOBEY_MS        5000u
#define BMS_CHARGE_CHARGER_STOPS_MS  5000u  /* the charger's own timeout (Elcon p.6) */
#define BMS_CHARGE_PACK_CELLS        128u   /* the request voltage: this x the target */

typedef enum {
    BMS_CHARGE_OFF,      /* not Charge mode: in the car */
    BMS_CHARGE_WAITING,
    BMS_CHARGE_CHARGING,
    BMS_CHARGE_COMPLETE,
} bms_charge_state_t;

/* Why the charge left CHARGING (an EVENT_CHARGE Event's cause). */
typedef enum {
    BMS_CHARGE_CAUSE_NONE         = 0, /* it started */
    BMS_CHARGE_CAUSE_TARGET       = 1, /* Charge complete */
    BMS_CHARGE_CAUSE_SILENT       = 2, /* the charger timed out */
    BMS_CHARGE_CAUSE_BMS_NOT_OK   = 3, /* a BMS fault, or BMS_OK low */
    BMS_CHARGE_CAUSE_SENSE_OPEN   = 4, /* the Shutdown Circuit sense open */
} bms_charge_cause_t;

/* From the BMS parameters (bms_params.h), fixed until the next reset. */
typedef struct {
    uint16_t target_mv;     /* the Charge target, on the highest cell */
    uint16_t full_limit_da; /* the full (100%) Charge limit, 0.1 A */
    uint16_t v_band_mv;     /* the voltage taper starts this far under the target */
    int16_t  hot_full_dc;   /* 0.1 C: 100% at or below */
    int16_t  hot_zero_dc;   /* 0 at or above; over hot_full_dc */
    int16_t  cold_full_dc;  /* 100% at or above */
    int16_t  cold_zero_dc;  /* 0 at or below; under cold_full_dc */
} bms_charge_config_t;

typedef struct {
    uint16_t code;
    uint8_t  data[EVENT_DATA_SIZE];
} bms_charge_event_t;

/* A flag change, a charge state change and the rested cells: one step's
   most. */
#define BMS_CHARGE_EVENTS_MAX 3u

typedef struct {
    size_t             count;
    bms_charge_event_t events[BMS_CHARGE_EVENTS_MAX];
} bms_charge_events_t;

/* The charger disobeys verdict of one step. */
typedef enum {
    BMS_CHARGE_OBEYS,     /* reported current within its margin */
    BMS_CHARGE_OVER,      /* over it, for less than BMS_CHARGE_DISOBEY_MS */
    BMS_CHARGE_DISOBEYS,  /* over it for BMS_CHARGE_DISOBEY_MS: the fault */
} bms_charge_obey_t;

typedef struct {
    bool              charge_mode;
    bms_charge_obey_t obey;
    uint16_t          reported_da; /* the charger's output current as read */
} bms_charge_heard_t;

/* What bms_charge_step() needs, as the step has judged it. */
typedef struct {
    uint32_t now_ms;
    /* This step took a Sweep that read a cell and a sensor; summary is the
       last such Sweep's. */
    bool     swept;
    const bms_pack_summary_t *summary;
    bool     bms_ok;       /* the level written on BMS_OK this step */
    bool     fault_active; /* a BMS fault is active */
    bool     sdc_closed;   /* the Shutdown Circuit sense reads closed */
} bms_charge_in_t;

/* bms_charge_init() it; touch nothing inside. */
typedef struct {
    bms_charge_config_t config;
    bool     charge_mode;
    bool     heard;           /* the mailbox VALID at the last listen */
    uint8_t  flags;           /* the status flags last read VALID */
    bms_charge_state_t state;
    bool     starting;        /* the start conditions hold ... */
    uint32_t start_since_ms;  /* ... since */
    bool     ending;          /* the end condition holds ... */
    uint32_t end_since_ms;    /* ... since */
    uint32_t complete_ms;     /* when Charge complete began */
    bool     rested_logged;
    bool     swept;           /* a Sweep has come in */
    uint32_t last_sweep_ms;
    uint32_t fraction_ppm;    /* of the full charge limit */
    uint16_t limit_da;        /* the Charge limit, as last returned */
    bool     requested;       /* a request has gone ... */
    uint32_t request_ms;      /* ... when */
    uint16_t request_da;      /* its current; 0 for a stop */
    bool     over;            /* the charger over its request ... */
    uint32_t over_since_ms;   /* ... since */
} bms_charge_t;

void bms_charge_init(bms_charge_t *c, const bms_charge_config_t *config);

/*
 * The start of a step, before any fault is judged: reads the charger's
 * mailbox (charger, after this step's mailbox_update()), enters Charge
 * mode on its first good frame, adds an Event for each change of the
 * charger's status flags and judges whether it disobeys.
 */
bms_charge_heard_t bms_charge_listen(bms_charge_t *c, uint32_t now_ms, const mailbox_t *charger,
                                     bms_charge_events_t *events);

/* After the state and BMS_OK: the charge's start and end, and the rested
   cells. Returns the Charge limit, 0.1 A. */
uint16_t bms_charge_step(bms_charge_t *c, const bms_charge_in_t *in,
                         bms_charge_events_t *events);

/* The request to send now, as the charger's signals; the current it asks
   for is what bms_charge_listen() judges the charger against from now. */
void bms_charge_request(bms_charge_t *c, uint32_t now_ms, struct can_control_chg_request_t *req);

bms_charge_state_t bms_charge_state(const bms_charge_t *c);

/* "OFF", "WAITING", "CHARGING", "COMPLETE"; "?" otherwise. */
const char *bms_charge_state_name(bms_charge_state_t state);

#endif /* BMS_CHARGE_H */
