/*
 * What the VCU sends on the DAQ bus from its DAQ publisher (issue 46): the
 * VCU_DaqStatus frame, every 100 ms.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_DAQ_H
#define VCU_DAQ_H

#include <stdint.h>

#include "daq_status.h"

/* Packs VCU_DaqStatus from in. frame is CAN_DAQ_VCU_DAQ_STATUS_LENGTH
 * bytes. */
void vcu_daq_build_status(const daq_status_t *in, uint8_t *frame);

#endif /* VCU_DAQ_H */
