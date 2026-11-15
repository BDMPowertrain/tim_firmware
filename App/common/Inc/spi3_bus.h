#ifndef APP_INC_SPI3_BUS_H
#define APP_INC_SPI3_BUS_H

#include <stdbool.h>
#include <stdint.h>

/* spi3Mutex enforcement for the shared SPI3 bus (onboard IMU + mezzanine
   header, tied together on the wire by R27/R28/R29 — see CLAUDE.md and
   Documentation/tim-rtos-architecture.md section 7). Every driver that
   touches SPI3, imu.c included, must bracket its CS-low...CS-high
   transaction with spi3_bus_lock()/spi3_bus_unlock(). Nothing here
   stops a driver that forgets to — the mutex only works if every path
   actually goes through it. */

/* Creates spi3Mutex. Must run before any task that might call
   spi3_bus_lock() is created — see tasks_init(). */
void spi3_bus_init(void);

/* Blocks the calling task until it holds spi3Mutex, up to timeout_ms
   (0 = don't block if it's already held; osWaitForever's numeric value,
   0xFFFFFFFF, blocks indefinitely). Returns true once the bus is
   exclusively this task's; false on timeout. */
bool spi3_bus_lock(uint32_t timeout_ms);

/* Releases a lock taken with spi3_bus_lock(). Must be called by the
   same task that acquired it — FreeRTOS mutexes are owned, unlike
   semaphores, which is what lets priority inheritance work: if a
   lower-priority task is holding spi3Mutex and a higher-priority task
   (imuTask, at osPriorityAboveNormal) blocks waiting for it, FreeRTOS
   temporarily raises the holder's priority so it finishes and releases
   promptly instead of getting stuck behind medium-priority tasks that
   don't even use SPI3. */
void spi3_bus_unlock(void);

#endif /* APP_INC_SPI3_BUS_H */
