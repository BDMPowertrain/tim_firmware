/*
 * Bus termination (issue 52): one bus's termination relay, as its
 * Parameter wants it and as the relay is commanded now.
 *
 * At start-up the image loads its Parameters, takes the transceiver out
 * of standby, then writes the stored value to the relay
 * (bus_term_write_stored()). The Control bus is started only once the bus
 * has settled: more than BUS_TERM_SETTLE_MS after that write.
 *
 * The shell's override (bus_term_override()) moves the relay for bench
 * use. It never changes the stored value, which is the Parameter in force
 * until the next reset; `param` is how a person changes what the next
 * start-up writes.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock. The caller writes
 * the GPIO and hands in the time.
 */
#ifndef BUS_TERM_H
#define BUS_TERM_H

#include <stdbool.h>
#include <stdint.h>

/* How long the bus is given, from the termination write, before CAN1 is
   started. UNMEASURED: 5 ms is the spec's figure, not a reading taken on
   the TIM's relay (TLP172AM) and transceiver (ADM3057E). Measure it on
   the bench before relying on it. */
#define BUS_TERM_SETTLE_MS 5u

typedef struct {
    bool     stored;     /* the Parameter in force */
    bool     relay;      /* what the relay is commanded to now */
    bool     written;    /* the stored value has been written */
    uint32_t written_ms; /* when it was */
} bus_term_t;

/* Writes the stored value: the relay is commanded to it. Once, at
   start-up, after the transceiver has left standby. */
void bus_term_write_stored(bus_term_t *t, bool stored, uint32_t now_ms);

/* True once more than BUS_TERM_SETTLE_MS have passed since the stored
   value was written; false before it is. "More than", because a 1 ms tick
   can count a shorter time as BUS_TERM_SETTLE_MS. */
bool bus_term_settled(const bus_term_t *t, uint32_t now_ms);

/* The shell's bench override: the relay is commanded to on. The stored
   value is unchanged. */
void bus_term_override(bus_term_t *t, bool on);

/* True if the relay is not where the stored value put it. */
bool bus_term_overridden(const bus_term_t *t);

#endif /* BUS_TERM_H */
