#ifndef APP_COMMON_INC_RESET_INFO_H
#define APP_COMMON_INC_RESET_INFO_H

/* reset_info — what the image knows about why it restarted (ADR 0011,
   0013): the reset cause, the core debug enable bit, and the Breadcrumb,
   Crash record and count of resets that were not a Power-on reset, kept
   in the backup registers (TAMP_BKPxR). The layout and the rules for
   what is kept are reset_record.h logic; this file moves the words to and
   from the registers.

   The backup registers survive every reset that keeps VDD (RM0440
   38.3.4). After a Power-on reset the whole backup domain is reset in
   software (ES0430 2.2.5: a supply that dips into a 100 to 700 mV window
   can leave it holding garbage), which is also what clears the records. */

#include <stdbool.h>
#include <stdint.h>

#include "reset_record.h"

/* Reads and clears the reset cause, captures the core debug enable bit
   and rewrites the backup registers (reset_record_boot()). Call first in
   main(), before HAL_Init(): RCC_CSR is read before anything else. */
void reset_info_capture(void);

/* The core debug enable bit (DHCSR.C_DEBUGEN) as start-up found it: a
   debugger had been attached since the last power-on. The BMS treats a
   reset under it as a Power-on reset (ADR 0011). */
bool reset_info_debugger(void);

/* Writes the Breadcrumb to the backup registers. The control step only,
   at the start of each step and of each phase. */
void reset_info_breadcrumb(const breadcrumb_t *breadcrumb);

/* Writes the Crash record. The crash routine only (crash.h), with
   interrupts off; touches nothing but registers. */
void reset_info_crash(const crash_record_t *crash);

/* What reset_info_capture() found: the last reset's cause, Breadcrumb,
   Crash record and count. Written once before the scheduler starts. */
const reset_report_t *reset_info_report(void);

/* The shell's `lastreset`: cause, Breadcrumb, Crash record and count. */
void reset_info_print(void);

#endif /* APP_COMMON_INC_RESET_INFO_H */
