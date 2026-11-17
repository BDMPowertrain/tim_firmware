#ifndef APP_INC_WATCHDOG_H
#define APP_INC_WATCHDOG_H

/* watchdog — the independent watchdog, the dead-firmware path (ADR 0008,
   0013). 60 ms nominal (56.5 to 65 ms as the LSI moves between 34 and
   29.5 kHz), window off, in both images. Once started it cannot be
   stopped; only a reset does that.

   Who Kicks, and when (ADR 0013):
   - watchdog_start(), straight after GPIO init in main(), which reloads
     the counter once as part of the start;
   - start-up, at exactly two fixed points: at the end of main()'s
     peripheral init, and in tasks_init() just before the control step is
     created;
   - after that only the control step, once at the start of each step, and
     only if the previous step ran every phase in order (kick_gate.h).
   Nothing else calls watchdog_kick(). No start-up stage may run longer
   than about 50 ms between Kicks.

   The watchdog is frozen while the core is halted by a debugger (the
   DBGMCU freeze bit is set in every build), so a breakpoint does not
   reset the board. */

#include <stdbool.h>

/* Sets the debug-freeze bit, then starts and configures the watchdog.
   Call once, from main() straight after MX_GPIO_Init(). */
void watchdog_start(void);

/* True if watchdog_start() saw the configuration take. If not, the
   watchdog still runs, on its reset values (about 512 ms), and the image
   stays in STARTUP. */
bool watchdog_started(void);

/* Reloads the watchdog: a Kick. See above for the only callers. */
void watchdog_kick(void);

#endif /* APP_INC_WATCHDOG_H */
