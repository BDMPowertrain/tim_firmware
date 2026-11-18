#ifndef APP_BMS_INC_CELL_MONITOR_H
#define APP_BMS_INC_CELL_MONITOR_H

/* cell_monitor — the link to the cell monitors (issue 62): the BQ79600-Q1
   bridge on SPI3 and, behind it, the 16 BQ79616-Q1 stack devices. It
   carries out one step's plan from the chain sequencer (bq_chain.h) and
   moves bytes; every decision is in the logic.

   SPI3 runs mode 0 at 2.66 MHz with DMA both ways (CubeMX). It is the BMS
   Control step's alone, with no mutex (ADR 0010, issue 19): on the BMS
   image nothing else touches SPI3, and the IMU's chip select is held high
   by GPIO init so the IMU, fitted or not, never drives MISO.

   - nCS (PA6) is driven here; it initialises high.
   - SPI_RDY (PA7) is a rising-edge interrupt at priority 6. Its handler
     only sets a thread flag on the waiting task.
   - The wake ping is 914 zero bytes with nCS low, 2.75 ms (the bridge wants
     2.5-3 ms). MOSI (PC12) stays an SPI pin throughout.
   - A write waits for SPI_RDY high before it goes (BQ79600-Q1 data sheet
     SLUSDS1A Fig 7-17); a read sends its command, then reads each
     128-byte buffer the bridge says is ready, until the response is in or
     tWAIT_READ_MAX has passed (Fig 7-16). A read that times out keeps the
     buffers already read and sends a Comm Clear; it never reads while
     SPI_RDY is low.

   The calling task sleeps on thread flags during every transfer and every
   wait, set by the DMA completion and the SPI_RDY interrupts. Called only
   by the BMS Control step. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bq_chain.h"

typedef struct {
    bool           ok;   /* the ping and every command went; every read came in whole */
    const uint8_t *resp; /* the last read's bytes, valid until the next run */
    size_t         len;
} cell_monitor_result_t;

/* Carries out plan and reports how it went. Blocks the caller for at most
   bq_plan_worst_us(plan) plus a tick per wait. */
void cell_monitor_run(const bq_plan_t *plan, cell_monitor_result_t *out);

/* Reads that timed out since power-on. */
uint32_t cell_monitor_timeouts(void);

#endif /* APP_BMS_INC_CELL_MONITOR_H */
