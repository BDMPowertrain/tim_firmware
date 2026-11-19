/*
 * One Sweep (issue 62): the stack read that returns every cell voltage and
 * every cell temperature sensor reading in the pack, and its decoding.
 *
 * The pack is 16 BQ79616-Q1 stack devices of 8 cells each. Each device
 * answers with the 38 bytes from VCELL8_HI (0x0578) to GPIO8_LO (0x059D):
 * 8 cell voltages, BUSBAR, TSREF and the 8 GPIOs, to which the 8 Enepaq
 * temperature sensors of its module are wired (research for issue 3,
 * section 3, option A). That is 16 x 44 = 704 response bytes, about 7.5 ms
 * on the chain. The top of the stack answers first.
 *
 * A device whose frame did not arrive whole and with a good CRC has not
 * been read: its cells and sensors are 0 mV and its bit in devices_read is
 * clear. A CRC failure ends the decoding, since nothing after it can be
 * framed, and a device below a CRC-bad frame does not append its own
 * (BQ79616-Q1 data sheet SLUSE81F p. 57). A reading below zero, such as the
 * registers' reset value 0x8000 before the first conversion, is 0 mV too:
 * 0 mV is outside every valid range, so it is always a missing measurement.
 *
 * After the stack's answer comes the bridge's FAULT_SUMMARY (0x2100,
 * BQ79600-Q1 data sheet SLUSDS1A 7.5.15), read on its own in the same step
 * (issue 64): non-zero when the bridge holds an unmasked fault of its own or
 * a stack device reported one up the chain, which is what NFAULT shows.
 *

 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BQ_SWEEP_H
#define BQ_SWEEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bq_frame.h"

#define BQ_SWEEP_DEVICES          16u
#define BQ_SWEEP_CELLS_PER_DEVICE 8u
#define BQ_SWEEP_CELLS            (BQ_SWEEP_DEVICES * BQ_SWEEP_CELLS_PER_DEVICE)
/* devices_read with every device read. */
#define BQ_SWEEP_ALL_DEVICES      ((uint16_t)((1u << BQ_SWEEP_DEVICES) - 1u))

#define BQ_SWEEP_REG   0x0578u /* VCELL8_HI */
#define BQ_SWEEP_COUNT 38u     /* to GPIO8_LO */
/* The stack's answer. */
#define BQ_SWEEP_RESPONSE_LEN (BQ_SWEEP_DEVICES * (BQ_SWEEP_COUNT + BQ_RESPONSE_OVERHEAD))

#define BQ_SWEEP_BRIDGE_REG 0x2100u /* the bridge's FAULT_SUMMARY */
#define BQ_SWEEP_BRIDGE_LEN (1u + BQ_RESPONSE_OVERHEAD)
/* The stack's answer, then the bridge's. */
#define BQ_SWEEP_TOTAL_LEN  (BQ_SWEEP_RESPONSE_LEN + BQ_SWEEP_BRIDGE_LEN)

/* Index i is cell (i % 8) + 1 of device (i / 8) + 1; device 1 is the
   bottom of the stack, next to the bridge. temp_mv[i] is the sensor on that
   device's GPIO (i % 8) + 1. */
typedef struct {
    uint16_t cell_mv[BQ_SWEEP_CELLS];
    uint16_t temp_mv[BQ_SWEEP_CELLS];
    /* Bit d - 1 set: device d was read. */
    uint16_t devices_read;
    bool     bridge_read;  /* the bridge's FAULT_SUMMARY came back */
    uint8_t  bridge_fault; /* it, when it did; 0: no fault */
} bq_sweep_t;

/* Builds the Sweep's command, a stack read; returns its length. */
size_t bq_sweep_command(uint8_t out[BQ_FRAME_MAX]);

/* Decodes the len bytes received for the Sweep's two reads into out: the
   stack's answer, then from BQ_SWEEP_RESPONSE_LEN on, the bridge's. */
void bq_sweep_decode(const uint8_t *resp, size_t len, bq_sweep_t *out);

#endif /* BQ_SWEEP_H */
