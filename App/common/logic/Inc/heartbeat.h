/*
 * The heartbeat LED's blink decision (issue 47; issue 36, user story 41):
 * 1 Hz while there is no active fault, 5 Hz with one, and still while the
 * Control step is stalled, so a person in the paddock can read a board's
 * health without a laptop.
 *
 * Housekeeping calls this every 100 ms with the period boundary it woke on,
 * the Control step's step count from its published snapshot and whether the
 * step reports any active fault, and toggles the LED when it says so. A
 * toggle needs both a full half-period since the last one (500 ms normal,
 * 100 ms with a fault) and a step count that has moved since the last one.
 * A fault that appears part-way through a 500 ms half-period shows on the
 * next 100 ms tick.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#define HEARTBEAT_NORMAL_HALF_PERIOD_MS 500u /* 1 Hz */
#define HEARTBEAT_FAULT_HALF_PERIOD_MS  100u /* 5 Hz */

/* Zero-initialise at power-on: the LED then waits for the first step. */
typedef struct {
    uint32_t last_toggle_ms;
    uint32_t step_count_at_toggle;
} heartbeat_t;

/* True: toggle the LED now. */
bool heartbeat_update(heartbeat_t *hb, uint32_t now_ms, uint32_t step_count,
                      bool fault_active);

#endif /* HEARTBEAT_H */
