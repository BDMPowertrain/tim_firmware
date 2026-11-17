#ifndef APP_COMMON_LOGIC_INC_RESET_RECORD_H
#define APP_COMMON_LOGIC_INC_RESET_RECORD_H

/* reset_record — what an image keeps across a reset in the backup
   registers (spec #36, Watchdog and crash path): the Breadcrumb, the Crash
   record and the count of resets that were not a Power-on reset. This
   module owns the layout of those words; the image only copies them to and
   from the registers (reset_info.h). Logic (ADR 0002).

   At each start, reset_record_boot() turns the words found into the
   report of the last reset and rewrites them:
   - after a Power-on reset everything is cleared (ADR 0011); the count
     starts at 0;
   - after any other reset the count goes up by one, and the Breadcrumb and
     Crash record move into the report and are cleared in the registers, so
     each one is reported against the one reset that followed it;
   - words not in this layout (left by other code, or by a backup domain
     that missed its reset, ES0430 2.2.5) are reported as not ours and
     replaced, the count starting again at 1.

   The Breadcrumb is three register writes: a reset between two of them
   leaves a step count and an Overrun count one step apart. */

#include <stdbool.h>
#include <stdint.h>

#include "reset_cause.h"

/* Backup registers used, from TAMP_BKP0R on. */
#define RESET_RECORD_WORDS 9u

/* Breadcrumb phase values beyond the image's own phase indices. */
#define BREADCRUMB_PHASE_NONE  0xFFu /* no step has run since start-up */
#define BREADCRUMB_PHASE_START 0xFEu /* the step has Kicked, no phase yet */
#define BREADCRUMB_PHASE_END   0xFDu /* every phase ran; between steps */

/* Where the Control step last was. phase is the index of the phase
   running, or one of the BREADCRUMB_PHASE_ values. */
typedef struct {
    uint32_t step_count; /* the step's number, 0 for the first */
    uint8_t  phase;
    uint32_t overruns;   /* Overruns since start-up */
} breadcrumb_t;

/* What the crash routine found wrong (crash.h). */
typedef enum {
    CRASH_NONE = 0,
    CRASH_STACK_OVERFLOW,
    CRASH_HARD_FAULT,
    CRASH_MEM_MANAGE,
    CRASH_BUS_FAULT,
    CRASH_USAGE_FAULT,
    CRASH_NMI,
    CRASH_ASSERT,
    CRASH_ERROR_HANDLER, /* the generated Error_Handler() */
    CRASH_COMMAND,       /* the shell's `crash`, a bench test (ADR 0002) */
} crash_cause_t;

/* Characters of the task name kept; a longer name is cut. */
#define CRASH_TASK_CHARS 8u

typedef struct {
    crash_cause_t cause;
    char          task[CRASH_TASK_CHARS + 1u]; /* NUL-terminated */
    uint32_t      address; /* where the code was: the faulting PC, or the
                              caller of an assert or the error handler */
} crash_record_t;

typedef struct {
    reset_cause_t  cause;
    /* The words were in this layout. Always true after a Power-on reset,
       which reports nothing by definition. */
    bool           layout_found;
    uint32_t       non_power_on_resets; /* this one included */
    breadcrumb_t   breadcrumb;  /* phase NONE if no step ran */
    crash_record_t crash;       /* cause CRASH_NONE if no crash */
} reset_report_t;

/* At start-up: words holds what the backup registers held (all zero after a
   backup-domain reset) and is rewritten with what they must hold now. */
void reset_record_boot(uint32_t words[RESET_RECORD_WORDS], reset_cause_t cause,
                       reset_report_t *report);

/* Write the Breadcrumb or the Crash record into words. Each touches only its
   own words, RESET_RECORD_*_FIRST onwards, so the image need copy only
   those to the registers. */
#define RESET_RECORD_BREADCRUMB_FIRST 2u
#define RESET_RECORD_BREADCRUMB_COUNT 3u
#define RESET_RECORD_CRASH_FIRST      5u
#define RESET_RECORD_CRASH_COUNT      4u

void reset_record_put_breadcrumb(uint32_t words[RESET_RECORD_WORDS],
                                 const breadcrumb_t *breadcrumb);
void reset_record_put_crash(uint32_t words[RESET_RECORD_WORDS], const crash_record_t *crash);

/* "hard fault", "assert" and so on, for the shell; "?" if out of range. */
const char *crash_cause_name(crash_cause_t cause);

#endif /* APP_COMMON_LOGIC_INC_RESET_RECORD_H */
