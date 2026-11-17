#ifndef APP_COMMON_LOGIC_INC_RESET_CAUSE_H
#define APP_COMMON_LOGIC_INC_RESET_CAUSE_H

/* reset_cause — why the MCU last reset, from the reset-cause register
   RCC_CSR as start-up found it (ADR 0011, RM0440 7.4.28).

   The register has no power-on flag: switching GLV on and a supply sag
   below the brown-out threshold both set BOR. So "BOR set" is a Power-on
   reset, whatever else is set with it. The flags accumulate until
   software clears them, and an internal reset also pulses NRST, so the
   pin flag comes last: an internal cause beside it wins. Logic (ADR 0002):
   the value is handed in. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RESET_CAUSE_UNKNOWN = 0,     /* no flag set */
    RESET_CAUSE_POWER_ON,        /* BOR: GLV on, or a sag and recovery */
    RESET_CAUSE_WATCHDOG,        /* the independent watchdog */
    RESET_CAUSE_WINDOW_WATCHDOG, /* the window watchdog (not used) */
    RESET_CAUSE_SOFTWARE,        /* the crash routine, or the shell's reset */
    RESET_CAUSE_OPTION_LOAD,     /* option bytes loaded (Provisioning) */
    RESET_CAUSE_LOW_POWER,       /* an illegal low-power entry */
    RESET_CAUSE_PIN,             /* the NRST pin alone: a probe or a button */
} reset_cause_t;

/* Classifies RCC_CSR as read before anything cleared it. */
reset_cause_t reset_cause_classify(uint32_t rcc_csr);

/* True for a Power-on reset: the one cause that clears the Breadcrumb, the
   Crash record and the count of other resets (reset_record.h). */
bool reset_cause_is_power_on(reset_cause_t cause);

/* "power-on", "watchdog" and so on, for the shell; "?" if out of range. */
const char *reset_cause_name(reset_cause_t cause);

#endif /* APP_COMMON_LOGIC_INC_RESET_CAUSE_H */
