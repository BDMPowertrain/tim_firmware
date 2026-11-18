/*
 * The VCU's Data log Records (issue 61, ADR 0021): what its Control step
 * saw and decided, one Record per step, and the map that names the
 * mailboxes in it. The file format around them is data_log_record.h; the
 * decoder is tools/data_log_decode.py, changed in the same commit as any
 * layout here (test_vcu_log checks the two agree).
 *
 * DATA_LOG_VCU_STEP payload, VCU_LOG_STEP_SIZE bytes, little-endian:
 *   0   the DATA_LOG_STEP payload (data_log_step_encode()): step count,
 *       state, flags, step time, Overruns
 *   12  inputs (u8): bit 0 Shutdown Circuit input closed, 1 start button
 *       pressed, 2 CAN1 running, 3 bus-off latch set, 4 error passive,
 *       5 receive FIFO overflowed, 6 every analog input has a sample,
 *       7 Brake engaged
 *   13  decided (u8): bit 0 the Power limiter binds, 1 the lower ceiling is
 *       the Discharge limit's (else the target's, vcu_ceiling_t), 2 the
 *       0x0C0 was sent, 3 a fault clear (0x0C1) was sent, 4 VCU_Status was
 *       sent, 5 brake light, 6 fault LED, 7 Pedal check due
 *   14  averaged ADC counts (u16 each): APPS1, APPS2, brake 1, brake 2,
 *       supply 1, supply 2
 *   26  Pedal travel (u16 each, tenths of a percent): APPS1, APPS2
 *   30  APPS status (u8 each, vcu_sensor_status_t): APPS1, APPS2
 *   32  brake pressure (u16 each, counts above zero): brake 1, brake 2
 *   36  brake status (u8 each): brake 1, brake 2
 *   38  start blockers (u16, VCU_START_...)
 *   40  Torque cuts active (u16, bit (1 << vcu_cut_t))
 *   42  the driver's torque request (i16, 0.1 N.m)
 *   44  the torque in the 0x0C0 (i16, 0.1 N.m)
 *   46  the torque ceiling (i16, 0.1 N.m, after the trim)
 *   48  the Power ceiling (u32, W, before the trim)
 *   52  the trim (i32, ppm)
 *   56  the 0x0C0 as sent (8 bytes), zeros if none was sent
 *   64  mailbox count (u8): VCU_RX_COUNT
 *   65  each mailbox in vcu_rx_t order, 9 bytes: its state (u8,
 *       mailbox_state_t: 0 NODATA, 1 VALID, 2 INVALID), then the frame the
 *       step read (mailbox_read(): the last good frame if VALID, its
 *       Substitute otherwise), zero-padded to 8 bytes
 *
 * DATA_LOG_MAILBOXES payload, VCU_LOG_MAP_SIZE bytes: count (u8), then each
 * mailbox's CAN ID (u32) in the same order, DATA_LOG_ID_EXTENDED set for a
 * 29-bit ID. The logger writes it first in every file, so a file names its
 * own mailboxes.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_LOG_H
#define VCU_LOG_H

#include <stdint.h>

#include "data_log_record.h"
#include "vcu_control.h"

#define VCU_LOG_MAILBOX_AT 65u
#define VCU_LOG_STEP_SIZE  (VCU_LOG_MAILBOX_AT + 9u * (unsigned)VCU_RX_COUNT)
#define VCU_LOG_MAP_SIZE   (1u + 4u * (unsigned)VCU_RX_COUNT)

/* common: what the runner knows of the step; in and decided: what
   vcu_control_step() was handed and gave back, in.mailboxes as the step
   read them. */
void vcu_log_step_encode(uint8_t out[VCU_LOG_STEP_SIZE], const data_log_step_t *common,
                         const vcu_control_in_t *in, const vcu_control_out_t *decided);

void vcu_log_map_encode(uint8_t out[VCU_LOG_MAP_SIZE]);

#endif /* VCU_LOG_H */
