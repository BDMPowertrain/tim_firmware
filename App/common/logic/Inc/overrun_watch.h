/*
 * What the Control step does with an Overrun beyond counting it (ADR 0013).
 *
 * An Overrun that still finishes still Kicks the watchdog on its next step,
 * so the watchdog never sees it. Three Overruns within one second are a timing fault, which
 * the image's logic acts on: a BMS fault on the BMS ("BMS internal fault"),
 * a Torque cut on the VCU ("timing fault"). The fault is reported once, on
 * the Overrun that makes three; each further Overrun with two others in the
 * second before it reports it again. What to latch, and for how long, is the
 * image's.
 *
 * Each Overrun is also one Event's worth of data: the step's duration, its
 * slowest phase and that phase's duration.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef OVERRUN_WATCH_H
#define OVERRUN_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OVERRUN_FAULT_COUNT     3u    /* Overruns ... */
#define OVERRUN_FAULT_WINDOW_MS 1000u /* ... within this long are a fault */

/* Zero-initialise at power-on. */
typedef struct {
    uint32_t at_ms[OVERRUN_FAULT_COUNT - 1u]; /* the last Overruns, oldest first */
    uint32_t seen;                            /* Overruns recorded, saturating */
} overrun_watch_t;

/*
 * Records an Overrun, with the time its step finished. True if it is the
 * third within OVERRUN_FAULT_WINDOW_MS: the timing fault.
 */
bool overrun_watch_record(overrun_watch_t *watch, uint32_t now_ms);

/* One Overrun Event's data. Times in microseconds. */
typedef struct {
    uint32_t step_us;
    uint32_t slowest_phase_us;
    uint8_t  slowest_phase; /* counted from 0; the first of equals */
    bool     extra_work;    /* the image's occasional extra work ran in the step */
} overrun_event_t;

/* The Event data of a step that overran, from its own timings and whether
   the image's occasional extra work ran in it (control_step.h). */
overrun_event_t overrun_event_make(uint32_t step_us, const uint32_t *phase_us,
                                   size_t phase_count, bool extra_work);

#endif /* OVERRUN_WATCH_H */
