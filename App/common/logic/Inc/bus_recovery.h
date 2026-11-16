/*
 * Bus recovery (ADR 0004, ADR 0005): what an image does when a CAN
 * controller goes bus-off, and the latch for a bus that keeps failing.
 *
 * Rules:
 * - A bus-off is reported at once (force every mailbox of the bus INVALID
 *   in this step) and the controller is restarted
 *   BUS_RECOVERY_RESTART_MS after the step that saw it.
 * - Clean time runs from the first step that sees the controller rejoined
 *   after a restart (its recovery takes 129 bus-idle sequences, and a bus
 *   held dominant never finishes it) until the next bus-off.
 * - A bus-off after less than BUS_RECOVERY_CLEAN_MS clean continues a run
 *   of quick bus-offs; one after more starts a new run. The
 *   BUS_RECOVERY_LATCH_BUS_OFFS-th bus-off of a run latches.
 * - While latched the restart waits BUS_RECOVERY_SLOW_RESTART_MS, so the
 *   bus is retried once per second.
 * - The latch clears after BUS_RECOVERY_UNLATCH_MS clean, on a step where
 *   the image's own condition holds (the VCU: every mailbox VALID).
 * - Error-passive is counted and reported, and changes nothing else.
 *
 * The controller tells the bus-offs apart by a count its interrupt keeps,
 * so a bus that fails again between two steps is still seen as a second
 * bus-off. Steps see a bus-off at most one period late, and the restart
 * and clean times are measured from the steps.
 *
 * Time is a 32-bit millisecond count compared only by subtraction, so the
 * wrap is harmless. Call bus_recovery_update() every step.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BUS_RECOVERY_H
#define BUS_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#define BUS_RECOVERY_RESTART_MS      10u
#define BUS_RECOVERY_SLOW_RESTART_MS 1000u
#define BUS_RECOVERY_CLEAN_MS        1000u
#define BUS_RECOVERY_LATCH_BUS_OFFS  3u
#define BUS_RECOVERY_UNLATCH_MS      5000u

/* What the step reads from the controller. */
typedef struct {
    uint32_t bus_off_count;   /* bus-offs signalled since power-on, kept by the interrupt */
    bool     controller_off;  /* the controller has not yet finished its recovery */
    bool     error_passive;   /* the controller is error-passive now */
    bool     latch_may_clear; /* the image's own condition for clearing the latch */
} bus_recovery_input_t;

/* What the step must do now. */
typedef struct {
    bool bus_off; /* a new bus-off: force every mailbox of the bus INVALID */
    bool restart; /* restart the controller */
} bus_recovery_action_t;

typedef struct {
    uint32_t bus_offs;       /* since power-on */
    uint32_t error_passives; /* times the controller became error-passive */
    uint32_t restarts;
} bus_recovery_counters_t;

/* Zero-initialise: running, not latched, no bus-off seen. */
typedef struct {
    bool     off;            /* bus-off and not yet restarted: send nothing */
    bool     latched;        /* the third quick bus-off; retrying once per second */
    bool     error_passive;  /* as last read */
    bool     running;        /* restarted and rejoined: running_since_ms is meaningful */
    uint8_t  quick_bus_offs; /* bus-offs in the current run */
    uint32_t off_since_ms;   /* the step that saw the bus-off */
    uint32_t running_since_ms; /* the first step that saw the controller rejoined */
    uint32_t seen_bus_off_count;
    bus_recovery_counters_t counters;
} bus_recovery_t;

/*
 * Run once per step with what the controller says now. Returns what the
 * step must do in this step: report a new bus-off, or restart.
 */
bus_recovery_action_t bus_recovery_update(bus_recovery_t *br,
                                          const bus_recovery_input_t *in,
                                          uint32_t now_ms);

#endif /* BUS_RECOVERY_H */
