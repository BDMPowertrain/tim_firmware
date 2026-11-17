/* reset_record.c — see reset_record.h. */

#include "reset_record.h"

#include <string.h>

/* The layout, one word each unless noted:
     0  magic: this layout, written at every start
     1  resets that were not a Power-on reset
     2  Breadcrumb step count
     3  Breadcrumb phase, stored + 1 so that an erased word reads as NONE
     4  Breadcrumb Overrun count
     5  Crash cause
     6  Crash address
     7, 8  Crash task, 8 characters, little-endian, NUL-padded */
#define W_MAGIC      0u
#define W_COUNT      1u
#define W_STEP       (RESET_RECORD_BREADCRUMB_FIRST + 0u)
#define W_PHASE      (RESET_RECORD_BREADCRUMB_FIRST + 1u)
#define W_OVERRUNS   (RESET_RECORD_BREADCRUMB_FIRST + 2u)
#define W_CAUSE      (RESET_RECORD_CRASH_FIRST + 0u)
#define W_ADDRESS    (RESET_RECORD_CRASH_FIRST + 1u)
#define W_TASK       (RESET_RECORD_CRASH_FIRST + 2u)

_Static_assert(RESET_RECORD_CRASH_FIRST + RESET_RECORD_CRASH_COUNT == RESET_RECORD_WORDS,
               "the crash record ends the layout");
_Static_assert(RESET_RECORD_BREADCRUMB_FIRST + RESET_RECORD_BREADCRUMB_COUNT
                   == RESET_RECORD_CRASH_FIRST,
               "the breadcrumb is followed by the crash record");
_Static_assert(CRASH_TASK_CHARS == 2u * sizeof(uint32_t), "the task takes two words");

/* "TIM" and the layout version 1. Change the version with the layout. */
#define RECORD_MAGIC 0x54494D01u

static breadcrumb_t get_breadcrumb(const uint32_t words[RESET_RECORD_WORDS])
{
    /* An erased phase word is 0, which decodes to 0xFF: NONE. */
    return (breadcrumb_t){
        .step_count = words[W_STEP],
        .phase = (uint8_t)(words[W_PHASE] - 1u),
        .overruns = words[W_OVERRUNS],
    };
}

void reset_record_put_breadcrumb(uint32_t words[RESET_RECORD_WORDS],
                                 const breadcrumb_t *breadcrumb)
{
    words[W_STEP] = breadcrumb->step_count;
    words[W_PHASE] = (uint32_t)(uint8_t)(breadcrumb->phase + 1u);
    words[W_OVERRUNS] = breadcrumb->overruns;
}

static crash_record_t get_crash(const uint32_t words[RESET_RECORD_WORDS])
{
    crash_record_t crash = {
        .cause = (crash_cause_t)words[W_CAUSE],
        .address = words[W_ADDRESS],
    };

    for (uint32_t i = 0; i < CRASH_TASK_CHARS; i++) {
        crash.task[i] = (char)(words[W_TASK + i / 4u] >> (8u * (i % 4u)));
    }
    crash.task[CRASH_TASK_CHARS] = '\0';
    return crash;
}

void reset_record_put_crash(uint32_t words[RESET_RECORD_WORDS], const crash_record_t *crash)
{
    uint32_t task[2] = { 0u, 0u };

    for (uint32_t i = 0; i < CRASH_TASK_CHARS && crash->task[i] != '\0'; i++) {
        task[i / 4u] |= (uint32_t)(uint8_t)crash->task[i] << (8u * (i % 4u));
    }
    words[W_CAUSE] = (uint32_t)crash->cause;
    words[W_ADDRESS] = crash->address;
    words[W_TASK] = task[0];
    words[W_TASK + 1u] = task[1];
}

void reset_record_boot(uint32_t words[RESET_RECORD_WORDS], reset_cause_t cause,
                       reset_report_t *report)
{
    const bool powerOn = reset_cause_is_power_on(cause);
    const bool found = words[W_MAGIC] == RECORD_MAGIC;
    static const breadcrumb_t noBreadcrumb = { .phase = BREADCRUMB_PHASE_NONE };
    static const crash_record_t noCrash = { .cause = CRASH_NONE };

    memset(report, 0, sizeof(*report));
    report->cause = cause;
    report->layout_found = powerOn || found;
    report->breadcrumb = noBreadcrumb;
    report->crash = noCrash;

    if (!powerOn) {
        if (found) {
            report->breadcrumb = get_breadcrumb(words);
            report->crash = get_crash(words);
        }
        report->non_power_on_resets = (found ? words[W_COUNT] : 0u) + 1u;
    }

    words[W_MAGIC] = RECORD_MAGIC;
    words[W_COUNT] = report->non_power_on_resets;
    reset_record_put_breadcrumb(words, &noBreadcrumb);
    reset_record_put_crash(words, &noCrash);
}

const char *crash_cause_name(crash_cause_t cause)
{
    switch (cause) {
    case CRASH_NONE:           return "none";
    case CRASH_STACK_OVERFLOW: return "stack overflow";
    case CRASH_HARD_FAULT:     return "hard fault";
    case CRASH_MEM_MANAGE:     return "memory management fault";
    case CRASH_BUS_FAULT:      return "bus fault";
    case CRASH_USAGE_FAULT:    return "usage fault";
    case CRASH_NMI:            return "NMI";
    case CRASH_ASSERT:         return "assert";
    case CRASH_ERROR_HANDLER:  return "error handler";
    case CRASH_COMMAND:        return "crash command";
    default:                   return "?";
    }
}
