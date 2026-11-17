#ifndef APP_INC_TERMINATION_H
#define APP_INC_TERMINATION_H

/* termination — the two CAN termination relays (issue 52): Can_Term_1
   (PC9) on the Control bus, Can_Term_2 (PC13) on the DAQ bus. Each closes
   a TLP172AM relay across its bus: drive high, two 59R in series (~120R);
   drive low, open (polarity confirmed on hardware, a multimeter across
   CANH/CANL on both buses). The rules are bus_term.h logic.

   Start-up order, per bus: the Parameters are loaded, the bus driver
   takes the transceiver out of standby, then writes the stored value here
   (termination_write_stored()). The first Control step starts CAN1 a
   full step period after that write, and each image holds its period
   above BUS_TERM_SETTLE_MS (5 ms, UNMEASURED) with a _Static_assert, so
   no one waits for it.

   GPIO init (issue 52): CubeMX's generated MX_GPIO_Init() in
   targets/<image>/Core/Src/main.c still drives both relay pins low as it
   configures them, because CubeMX always gives an output pin a level.
   Low is open, the same as the reset state, so the Parameter is never
   applied there; this module's write is the first time it is. Making
   GPIO init leave the pins alone would mean changing tim.ioc, which is
   not done.

   The shell's `term` command overrides either relay for bench use and
   never changes the stored value. An override is refused whenever the
   image's bench refusal hook refuses (shell.h): the VCU in
   READY_TO_DRIVE. Any task may override; only the bus
   drivers write the stored value, once each, from tasks_init(). */

#include <stdbool.h>

typedef enum {
    TERMINATION_CONTROL_BUS, /* CAN1 */
    TERMINATION_DAQ_BUS,     /* CAN2 */
    TERMINATION_BUS_COUNT,
} termination_bus_t;

/* Commands the bus's relay to its Parameter in force. Once per bus,
   after the transceiver has left standby. */
void termination_write_stored(termination_bus_t bus, bool stored);

/* The bench override: the relay to on, the stored value unchanged. */
void termination_override(termination_bus_t bus, bool on);

/* The relay as commanded now: not a measured fact. */
bool termination_relay(termination_bus_t bus);

/* The shell's `term` command: term [1|2 on|off|stored]. */
int termination_cmd(int argc, char **argv);

#endif /* APP_INC_TERMINATION_H */
