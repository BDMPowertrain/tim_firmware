/* cycle_counter.c — see cycle_counter.h. */

#include "cycle_counter.h"

#include "main.h"

void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t cycle_counter_read(void)
{
    return DWT->CYCCNT;
}
