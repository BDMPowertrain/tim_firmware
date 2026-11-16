#ifndef APP_INC_CYCLE_COUNTER_H
#define APP_INC_CYCLE_COUNTER_H

/* The DWT cycle counter: 170 MHz, wraps every 25 s, so an unsigned
   difference across anything shorter is right. Two users: the Control step
   times itself and its phases with it, and FreeRTOS's run-time statistics
   count each task's CPU time in it (FreeRTOSConfig.h,
   configGENERATE_RUN_TIME_STATS), which Housekeeping turns into a CPU share
   per task (stats.h). The kernel compares rather than subtracts, so it
   loses the one time slice that spans each wrap: at most about a
   millisecond in 25 s.

   Started and zeroed once, by the scheduler as it starts (portCONFIGURE_
   TIMER_FOR_RUN_TIME_STATS), before any task runs. Zeroing matters after a
   reset that is not a power-on reset, which leaves the counter running:
   the kernel takes the first task's time from zero. */

#include <stdint.h>

void cycle_counter_init(void);

uint32_t cycle_counter_read(void);

#endif /* APP_INC_CYCLE_COUNTER_H */
