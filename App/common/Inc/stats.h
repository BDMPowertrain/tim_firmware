#ifndef APP_INC_STATS_H
#define APP_INC_STATS_H

#include <stdbool.h>

/* Task statistics (issue 47): each task's name, state, priority, CPU share
   over the last second and stack high-water mark. Housekeeping gathers them
   once a second; the shell's `tasks` prints the latest gathering. */

/* Creates the lock between the two. Call before either runs. */
bool stats_init(void);

/* Housekeeping only: takes a new gathering. The CPU share is each task's
   part of the run time counted since the last gathering (FreeRTOS run-time
   statistics in the DWT cycle counter, cycle_counter.h). */
void stats_gather(void);

/* The shell's `tasks`: prints the latest gathering via shell_printf(). */
void stats_print_tasks(void);

#endif /* APP_INC_STATS_H */
