#ifndef APP_INC_PARAMETERS_H
#define APP_INC_PARAMETERS_H

/* parameters — the running image's Parameters (ADR 0014, 0015): its table,
   loaded once at start-up from bank 2, and the shell's `param` command
   that sets, saves and dumps them. Storage is params.h logic over
   flash_bank2. The image hands over its table; this file never checks
   which image it is built into.

   The values in force are read once by parameters_init() and never change
   until the next reset, so any task may read them without a lock. A set
   or save changes only what the next start-up loads. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "params.h"

/* Asked before every save. NULL allows it; otherwise the reason the save is
   refused, for the shell. Runs on the shell task. */
typedef const char *(*parameters_save_hook_t)(void);

/* Loads the values in force, printing nothing (it runs on initTask's
   small stack), and posts what it found as Events (events.h).
   save_hook may be NULL (saves always allowed). Call once from
   tasks_init(), after events_init() and before the shell or the control
   step runs. False if
   the table is unusable: every value is then 0 and nothing is saved. */
bool parameters_init(const param_table_t *table, parameters_save_hook_t save_hook);

/* Prints what parameters_init() found: loaded, "parameters defaulted" or
   not dual-bank, and how many values were out of range. The shell task
   calls it once when it starts. */
void parameters_print_report(void);

/* The value in force of the parameter at index into the image's table. */
int32_t parameters_value(size_t index);

/* "Parameters defaulted": no valid copy was loaded, or a value in it was
   missing or out of range (params_defaulted()). */
bool parameters_defaulted(void);

/* The value in force came from the copy loaded, not a default. */
bool parameters_from_copy(size_t index);

/* The save counter of the copy loaded, 0 if none was (ADR 0016: a Pedal
   check pass is for these Parameters). */
uint32_t parameters_save_counter(void);

/* Sets the pending values of count Parameters, calibrations included, and
   saves, through the save hook: the image's guided capture saves a
   calibration this way, as `param set` refuses one. Anything else pending
   is saved with them. NULL once saved, otherwise why not, for the shell.
   Shell task only. */
const char *parameters_save_values(const size_t *index, const int32_t *value, size_t count);

/* The shell's `param` command: param [dump] | set <name> <value> | save.
   A calibration Parameter is never set by hand, and `param dump` prints
   the calibration as one record after the rest. */
int parameters_cmd(int argc, char **argv);

#endif /* APP_INC_PARAMETERS_H */
