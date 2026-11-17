#ifndef APP_INC_TERMINATION_H
#define APP_INC_TERMINATION_H

/* termination — the two CAN termination relays (issue 52): Can_Term_1
   (PC9) on the Control bus, Can_Term_2 (PC13) on the DAQ bus. Each closes
   a TLP172AM relay across its bus: drive high, two 59R in series (~120R);
   drive low, open (polarity confirmed on hardware, a multimeter across
   CANH/CANL on both buses). The rules are bus_term.h logic.

   Start-up order, per bus: the Parameters are loaded, the bus driver
   takes the transceiver out of standby, then writes the stored value here
   (termination_write_stored()). CubeMX's GPIO init leaves both relays
   open, the reset state; only that write closes one at start-up. The
   Control bus is started only once termination_settled(), more than
   BUS_TERM_SETTLE_MS (5 ms, UNMEASURED) after the write: the first Control
   step comes a full step period after it, so no one waits for it.

   The shell's `term` command overrides either relay for bench use and
   never changes the stored value. Any task may override; only the bus
   drivers write the stored value, once each, from tasks_init(). */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TERMINATION_CONTROL_BUS, /* CAN1 */
    TERMINATION_DAQ_BUS,     /* CAN2 */
    TERMINATION_BUS_COUNT,
} termination_bus_t;

/* Commands the bus's relay to its Parameter in force and starts the
   settle time. Once per bus, after the transceiver has left standby. */
void termination_write_stored(termination_bus_t bus, bool stored);

/* True once the bus's stored value was written more than
   BUS_TERM_SETTLE_MS before now_ms. */
bool termination_settled(termination_bus_t bus, uint32_t now_ms);

/* The bench override: the relay to on, the stored value unchanged. */
void termination_override(termination_bus_t bus, bool on);

/* The relay as commanded now: not a measured fact. */
bool termination_relay(termination_bus_t bus);

/* The shell's `term` command: term [1|2 on|off|stored]. */
int termination_cmd(int argc, char **argv);

#endif /* APP_INC_TERMINATION_H */
