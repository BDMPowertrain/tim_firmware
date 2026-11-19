/*
 * The BMS's Data log Records (issue 69, ADR 0021): what its Control step
 * saw and decided. Every step writes the pack Record to the step ring; a
 * step whose Sweep is every fifth (10 Hz) also writes the snapshot Record
 * to the snapshot ring, 16 deep (1.6 s). The snapshot starts with the same
 * step's pack payload, so it stands alone. The BMS's card is the only
 * record of a charge, since no VCU is present then.
 *
 * The file format around them is data_log_record.h; the decoder is
 * tools/data_log_decode.py, changed in the same commit as any layout here
 * (test_bms_log checks the two agree).
 *
 * DATA_LOG_BMS_PACK payload, BMS_LOG_PACK_SIZE bytes, little-endian:
 *   0   the DATA_LOG_STEP payload (data_log_step_encode()): step count,
 *       state, flags, step time, Overruns
 *   12  inputs (u8): bit 0 this step took a Sweep, 1 Shutdown Circuit
 *       sense closed, 2 FB.NFAULT high (no fault), 3 FB.BMS_OK high,
 *       4 CAN1 running, 5 bus-off latch set, 6 error passive, 7 receive
 *       FIFO overflowed
 *   13  decided (u8): bit 0 BMS_OK written high, 1 under the low-voltage
 *       limit, 2 open-wire check due, 3 the charger's mailbox VALID, 4 pack
 *       current valid, 5 the bridge's FAULT_SUMMARY read, 6 a balancing
 *       command given, 7 spare
 *   14  BMS faults active (u16, bit (1 << bms_fault_t))
 *   16  BMS faults seen since power-on (u16)
 *   18  cell-monitor chain state (u8, bq_chain_state_t)
 *   19  charge state (u8, bms_charge_state_t)
 *   20  balancing State, 21 its Reason (u8 each, BMS_Balancing)
 *   22  devices read (u16, bit d - 1: device d)
 *   24  the bridge's FAULT_SUMMARY (u8), 0 if not read
 *   25  cells read (u8), 26 sensors read in range (u8), 27 spare
 *   28  the last Sweep's summary (bms_pack_summary_t): pack voltage, the
 *       sum of the cells read (mV, u32); 32 lowest and 34 highest cell
 *       (mV, u16 each); 36 lowest and 37 highest cell's index (u8 each,
 *       0-127, module = index / 8); 38 coolest and 40 hottest sensor
 *       (0.1 C, i16 each); 42 coolest and 43 hottest sensor's index (u8)
 *   44  pack current as the step took it (i16, 0.1 A, positive
 *       discharging)
 *   46  Discharge limit sent (u16, 0.1 A), 48 Charge limit sent (u16,
 *       0.1 A)
 *   50  the last settled Sweep's Cell spread (u16, mV)
 *
 * DATA_LOG_BMS_SNAPSHOT payload, BMS_LOG_SNAPSHOT_SIZE bytes:
 *   0   the DATA_LOG_BMS_PACK payload
 *   52  each cell's voltage (u16, mV), index 0-127 as above; 0 is not read
 *   308 each temperature sensor's reading (u16, mV) as the Sweep gave it,
 *       in the same order; outside 1300-2440 mV is not a valid
 *       temperature (cell_temp.h)
 *   564 the bleed flags (16 bytes): bit k of byte m, cell k of module m
 *       commanded on by Balancing
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_LOG_H
#define BMS_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_control.h"
#include "bq_chain.h"
#include "data_log_record.h"

#define BMS_LOG_PACK_SIZE     52u
#define BMS_LOG_CELLS_AT      BMS_LOG_PACK_SIZE
#define BMS_LOG_TEMPS_AT      (BMS_LOG_CELLS_AT + 2u * BQ_SWEEP_CELLS)
#define BMS_LOG_BLEED_AT      (BMS_LOG_TEMPS_AT + 2u * BQ_SWEEP_CELLS)
#define BMS_LOG_SNAPSHOT_SIZE (BMS_LOG_BLEED_AT + BQ_SWEEP_DEVICES)

/* Sweeps per snapshot: 10 Hz at one Sweep per 20 ms step. */
#define BMS_LOG_SNAPSHOT_SWEEPS 5u

/* True on a step whose Sweep is a snapshot's: swept, and the Sweeps taken
   since power-on, this one included, a multiple of
   BMS_LOG_SNAPSHOT_SWEEPS. */
bool bms_log_snapshot_due(bool swept, uint32_t sweeps);

/* common: what the runner knows of the step; chain: the chain's state at
   the end of it; in and decided: what bms_control_step() was handed and
   gave back, in.sweep the step's Sweep (all zero on a step that took
   none). */
void bms_log_pack_encode(uint8_t out[BMS_LOG_PACK_SIZE], const data_log_step_t *common,
                         bq_chain_state_t chain, const bms_control_in_t *in,
                         const bms_control_out_t *decided);

void bms_log_snapshot_encode(uint8_t out[BMS_LOG_SNAPSHOT_SIZE], const data_log_step_t *common,
                             bq_chain_state_t chain, const bms_control_in_t *in,
                             const bms_control_out_t *decided);

#endif /* BMS_LOG_H */
