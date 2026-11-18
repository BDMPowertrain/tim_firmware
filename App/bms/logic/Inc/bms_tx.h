/*
 * What the BMS control step sends on the Control bus, and when (ADR 0004,
 * ADR 0010). A frame is built from current values on the step it is due and
 * handed straight to a hardware send slot; nothing is queued.
 *
 * Send table (spec #36; issue 66), a five-step cycle: BMS_Status and
 * BMS_Power every step, from the first, plus one slower frame a step in
 * the order of bms_tx_slow_t, so each goes every 100 ms. That is three
 * frames a step, never more: the controller has three send slots and a
 * frame never waits for one (ADR 0004). 0x60D, every 100 ms, is inside the
 * Energy Meter's 250 ms. At the step count's wrap (after 994 days) one slot
 * repeats, delaying the others by one step.
 * In the BMS state CHARGE the charger request (CHG_Request, 0x1806E5F4)
 * takes the 0x202 slot on one step in fifty (bms_tx_charger_request_due(),
 * issue 67), so it goes once a second; can/control.dbc gives step 26 of
 * the fifty to the balancing status frame, which is not built yet.
 *
 * Every frame's layout and units are can/control.dbc's; this module is the
 * only place that fills one. The values come in as bms_tx_values_t, the
 * readings from one Sweep in brief (bms_pack_summary_t).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_TX_H
#define BMS_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_derate.h"
#include "bq_sweep.h"
#include "can_control.h"
#include "e2e_p11.h"

/* At most this many frames are due on a step: the hardware send slots. */
#define BMS_TX_FRAMES_MAX 3u

/* A classic CAN frame's most data bytes. */
#define BMS_TX_FRAME_DATA_MAX 8u

/* One frame built by the step, for control_bus_send(). */
typedef struct {
    uint32_t id;
    bool     extended;
    uint8_t  len;
    uint8_t  data[BMS_TX_FRAME_DATA_MAX];
} bms_tx_frame_t;

/* The slower frames, one a step, in this order. */
typedef enum {
    BMS_TX_SLOW_LIMITS,          /* BMS_Limits (0x081) */
    BMS_TX_SLOW_INVERTER_LIMITS, /* INV_BmsLimits (0x202) */
    BMS_TX_SLOW_EM_CELL_TEMPS,   /* EM_CellTemps (0x60D) */
    BMS_TX_SLOW_PACK_VOLTAGES,   /* BMS_PackVoltages (0x210) */
    BMS_TX_SLOW_PACK_TEMPS,      /* BMS_PackTemps (0x211) */
    BMS_TX_SLOW_COUNT,
    BMS_TX_SLOW_NONE = BMS_TX_SLOW_COUNT
} bms_tx_slow_t;

/* One Sweep's readings in brief, over the cells and sensors it read
   validly (a device read; a cell not 0 mV; a sensor in its range). A cell
   or sensor index runs 0 to 127: module index / 8, number index % 8. */
typedef struct {
    uint16_t cells;     /* cells read; 0: none, the rest of the cell fields 0 */
    uint32_t sum_mv;    /* the pack voltage: the sum of the cells read */
    uint16_t min_mv;
    uint16_t max_mv;
    uint8_t  min_cell;
    uint8_t  max_cell;
    uint16_t temps;     /* sensors read in range; 0: none */
    int32_t  sum_dc;    /* 0.1 C */
    int16_t  min_dc;
    int16_t  max_dc;
    uint8_t  min_temp;
    uint8_t  max_temp;
} bms_pack_summary_t;

/* What the frames other than BMS_Status carry. */
typedef struct {
    const bms_pack_summary_t *summary;   /* the last Sweep's */
    bms_current_t pack_current;          /* goes as 0 A while invalid (#29) */
    uint16_t      discharge_limit_da;    /* 0.1 A */
    uint16_t      charge_limit_da;       /* 0.1 A */
} bms_tx_values_t;

/* The E2E counters of the protected frames, one each. */
typedef struct {
    e2e_p11_tx_t status;
    e2e_p11_tx_t power;
    e2e_p11_tx_t limits;
} bms_tx_e2e_t;

/* step_count is 0 on the first step. */
bool bms_tx_status_due(uint32_t step_count);
bool bms_tx_power_due(uint32_t step_count);
bms_tx_slow_t bms_tx_slow_due(uint32_t step_count);

/* The 0x202 slot of step 1 of each 50-step second: the charger request's,
   in CHARGE. */
bool bms_tx_charger_request_due(uint32_t step_count);

/* CHG_Request, 29-bit, in the charger's big-endian format; no E2E. */
void bms_tx_build_charger_request(const struct can_control_chg_request_t *r,
                                  bms_tx_frame_t *f);

/* Sums up one Sweep. */
void bms_tx_summarise(const bq_sweep_t *sweep, bms_pack_summary_t *out);

/*
 * Packs the BMS_Status signals (crc and counter are ignored) into f and
 * protects it with E2E Profile 11, advancing e2e->status.
 */
void bms_tx_build_status(const struct can_control_bms_status_t *signals,
                         bms_tx_e2e_t *e2e, bms_tx_frame_t *f);

/* BMS_Power (0x082), E2E-protected: the Discharge limit, pack current and
   pack voltage. */
void bms_tx_build_power(const bms_tx_values_t *v, bms_tx_e2e_t *e2e, bms_tx_frame_t *f);

/* The slower frame slow (not BMS_TX_SLOW_NONE). 0x202 carries the
   Discharge limit in whole amps, rounded up, so the inverter's backup sits
   at or just above the limit the VCU obeys (ADR 0017). 0x60D carries no
   temperature below 0 C (can/control.dbc). */
void bms_tx_build_slow(bms_tx_slow_t slow, const bms_tx_values_t *v, bms_tx_e2e_t *e2e,
                       bms_tx_frame_t *f);

#endif /* BMS_TX_H */
