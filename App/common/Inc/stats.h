#ifndef APP_INC_STATS_H
#define APP_INC_STATS_H

/* Prints one snapshot of every task currently known to the scheduler —
   name, state, priority, and stack high-water mark — via shell_printf().
   Called from the shell's `tasks` command. Safe to call from any task:
   uxTaskGetSystemState() (the FreeRTOS call this wraps) briefly suspends
   the scheduler internally while it walks the task lists, so it can't
   race a task being created or deleted out from under it. */
void stats_print_tasks(void);

#endif /* APP_INC_STATS_H */