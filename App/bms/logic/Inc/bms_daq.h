/*
 * What the BMS sends on the DAQ bus from its DAQ publisher (issue 46): the
 * BMS_DaqStatus frame, every 100 ms. The module summaries and cell dumps
 * join it with the Sweep (issue 69).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_DAQ_H
#define BMS_DAQ_H

#include <stdint.h>

#include "daq_status.h"

/* Packs BMS_DaqStatus from in. frame is CAN_DAQ_BMS_DAQ_STATUS_LENGTH
 * bytes. */
void bms_daq_build_status(const daq_status_t *in, uint8_t *frame);

#endif /* BMS_DAQ_H */
