#ifndef APP_INC_HOUSEKEEPING_H
#define APP_INC_HOUSEKEEPING_H

/* Housekeeping (issue 47): the one Low-priority task in each image, every
   100 ms. It bumps a counter the Control step watches (stall_watch.h),
   drives the heartbeat LED (heartbeat.h), runs the image's own slow
   reading (VBAT on the VCU) and once a second gathers each task's CPU
   share and stack high-water mark for the shell (stats.h).

   Being the lowest task, it is the first to starve: the step raises
   "background stalled" when the counter stops for 1 s, and the LED stops
   when the step itself stops. */

#include <stdbool.h>
#include <stdint.h>

#define HOUSEKEEPING_PERIOD_MS 100u

/* The argument of housekeepingTask(). Must outlive the task. */
typedef struct {
    /* The image's own work every period, or NULL. It must be short: it
       delays the heartbeat. */
    void (*poll)(void);
} housekeeping_def_t;

/* Creates what the task needs. Call from tasks_init() before the task is
   created. False if anything could not be created. */
bool housekeeping_init(void);

/* The task entry point. argument is a const housekeeping_def_t *. */
void housekeepingTask(void *argument);

/* Runs of the task since power-on; wraps. Read by the Control step. */
uint32_t housekeeping_count(void);

#endif /* APP_INC_HOUSEKEEPING_H */
