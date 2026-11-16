/*
 * The DAQ bus bus-off policy (issue 36, "DAQ bus isolation"): a CAN2
 * bus-off is retried once per second, forever, and affects nothing else.
 * There is no latch and no quick restart: the DAQ bus carries no safety
 * function, so the Control bus's policy (bus_recovery.h) does not apply.
 *
 * The caller polls with whether the controller is off now (in an image,
 * FDCAN2's CCCR.INIT, which the controller sets at bus-off and the restart
 * clears) and restarts it when this says so. A restart that does not take
 * is retried one second later. Time is a 32-bit millisecond count compared
 * only by subtraction, so the wrap is harmless.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef DAQ_RETRY_H
#define DAQ_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#define DAQ_RETRY_MS 1000u

/* Zero-initialise: running, nothing seen. */
typedef struct {
    bool     off;           /* off as of the last poll */
    uint32_t off_since_ms;  /* the poll that saw it off, or the last restart */
    uint32_t bus_offs;      /* times seen going off, since power-on */
    uint32_t restarts;
} daq_retry_t;

/* Polls with the controller's state at now_ms. True: restart it now. */
bool daq_retry_update(daq_retry_t *r, bool controller_off, uint32_t now_ms);

#endif /* DAQ_RETRY_H */
