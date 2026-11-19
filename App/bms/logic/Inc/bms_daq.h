/*
 * What the BMS sends on the DAQ bus from its DAQ publisher (issues 46 and
 * 69), every 100 ms publish, all from a copy of the published snapshot:
 *
 *   - BMS_DaqStatus (0x280), every publish;
 *   - BMS_ModuleSummary_nn (0x610 + module), every 200 ms: the odd modules
 *     on odd publishes, the even on even;
 *   - the per-cell dump, every 1 s: BMS_CellVoltagesA/B_nn and
 *     BMS_CellTemps_nn (0x620 + 3 x module and the two after it), module
 *     m's on publishes where publish % 10 == m % 10.
 *
 * So no publish queues more than BMS_DAQ_FRAMES_MAX frames and the status
 * frame, which the DAQ bus send queue (frame_queue.h) holds whole. Missing
 * readings are as can/daq.dbc's comments say: a cell not read is 0 V, a
 * sensor not read validly is -3276.8 C in the dump and left out of the
 * summary, whose half with nothing read validly is all zeros.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_DAQ_H
#define BMS_DAQ_H

#include <stddef.h>
#include <stdint.h>

#include "bq_sweep.h"
#include "can_frame.h"
#include "daq_status.h"

/* The most frames one publish gives (bms_daq_build_frames()). */
#define BMS_DAQ_FRAMES_MAX 14u

/* Packs BMS_DaqStatus from in. frame is CAN_DAQ_BMS_DAQ_STATUS_LENGTH
 * bytes. */
void bms_daq_build_status(const daq_status_t *in, uint8_t *frame);

/* The summaries and dump frames due on the publish'th publish (0 the
 * first), from sweep and bleeding (bit k of bleeding[m]: cell k of module m
 * bled by Balancing), into out. Each frame's ID, length and data are set,
 * the rest false. Gives at most max frames, never part of a module's dump;
 * returns how many. */
size_t bms_daq_build_frames(uint32_t publish, const bq_sweep_t *sweep,
                            const uint8_t bleeding[BQ_SWEEP_DEVICES],
                            can_frame_t *out, size_t max);

#endif /* BMS_DAQ_H */
