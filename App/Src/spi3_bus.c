/* spi3_bus.c — owns spi3Mutex. This is the whole implementation: a
   mutex, and two functions that acquire/release it. Everything that
   makes it matter lives in the drivers that call it, not here. */

#include "spi3_bus.h"

#include "cmsis_os2.h"

#include "main.h"

static osMutexId_t spi3Mutex;

void spi3_bus_init(void)
{
    const osMutexAttr_t spi3Mutex_attributes = {
        .name = "spi3Mutex",
    };

    spi3Mutex = osMutexNew(&spi3Mutex_attributes);
    if (spi3Mutex == NULL) {
        Error_Handler();
    }
}

bool spi3_bus_lock(uint32_t timeout_ms)
{
    return osMutexAcquire(spi3Mutex, timeout_ms) == osOK;
}

void spi3_bus_unlock(void)
{
    osMutexRelease(spi3Mutex);
}
