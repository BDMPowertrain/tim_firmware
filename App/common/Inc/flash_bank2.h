#ifndef APP_INC_FLASH_BANK2_H
#define APP_INC_FLASH_BANK2_H

/* flash_bank2 — the flash interface (flash_if.h) on the stored-data
   regions of bank 2 (ADR 0014), over the HAL flash functions only: they
   carry the workaround for erratum ES0430 2.2.2.

   A read of a double word left undefined by a cut write raises a double
   ECC error, which is an NMI. NMI_Handler calls flash_bank2_nmi() first:
   for a failing address in the bank 2 data regions it clears the error,
   marks the read failed and returns, so the reader skips that copy or
   Event. Any other NMI is not ours, and the handler goes on to the crash
   routine. */

#include <stdbool.h>

#include "flash_if.h"

/* Bank 2's data regions: the two Parameter copies and the Event log. */
#define FLASH_BANK2_DATA_START 0x08040000u
#define FLASH_BANK2_DATA_END   0x08052000u /* one past the last byte */

/* Creates the mutex every read, erase and program then holds, so the
   shell and the Event writer can share the adapter. Call from tasks_init()
   before any task that uses it is created. False if it could not be
   created. */
bool flash_bank2_init(void);

/* The adapter, or NULL if the part is not dual-bank: then nothing may be
   written, and the image runs on defaults. */
const flash_if_t *flash_bank2_get(void);

/* From NMI_Handler, first. True if the NMI was a double ECC error in the
   bank 2 data regions: cleared and recorded, and the handler returns. */
bool flash_bank2_nmi(void);

#endif /* APP_INC_FLASH_BANK2_H */
