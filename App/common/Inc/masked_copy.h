#ifndef APP_INC_MASKED_COPY_H
#define APP_INC_MASKED_COPY_H

/* The one way data crosses into and out of the Control step (ADR 0010):
   a copy made with interrupts masked, so that neither side ever sees a
   half-written value. The masked section holds the copy and nothing else.
   It masks every interrupt, the CAN1 receive interrupt included, so keep
   what is copied small: the largest planned is the BMS published
   snapshot, about 500 bytes. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "main.h"

static inline void masked_copy(void *dst, const void *src, size_t len)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    memcpy(dst, src, len);
    __set_PRIMASK(primask);
}

#endif /* APP_INC_MASKED_COPY_H */
