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
   small stack). save_hook may be NULL (saves always allowed). Call once
   from tasks_init(), before the shell or the control step runs. False if
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

/* The shell's `param` command: param [dump] | set <name> <value> | save. */
int parameters_cmd(int argc, char **argv);

#endif /* APP_INC_PARAMETERS_H */
