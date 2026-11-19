#ifndef APP_COMMON_INC_DATA_LOG_H
#define APP_COMMON_INC_DATA_LOG_H

/* data_log — the Data log's SD logger (ADR 0021, issue 53): the one task
   that reads the rings (data_log_ring.h) and the only one that calls
   FatFS. It replaces the CSV logger and its start, stop and dump commands.

   It waits for the end of STARTUP, then mounts the card and opens a new
   file in this boot's directory (data_log_card.h), writes the image's
   preamble Record into it, and from then on
   encodes every Record (data_log_record.h) into a 4 KB batch that goes to
   the card in whole sectors (log_batch.h), syncing every second (the bytes
   after the last whole sector wait in RAM for the next) and rolling the
   file every 10 minutes. Before opening a file, and when a write finds the
   card full, it deletes the oldest files until 10% of the card is free.

   A missing, removed or failing card closes the file, sets the card state
   (in the DAQ status frame), writes one Event and retries every 5 s into a
   new file; meanwhile the logger keeps emptying the rings and counts what
   it could not write. Nothing here is a fault, and nothing here changes
   what the Control step does: the step only pushes to its ring.

   tools/data_log_decode.py turns a file into CSV.

   Common code, linked into an image that has the SD card's SPI2 and FatFS
   configured. This file never checks which image it is built into. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data_log_ring.h"

/* The image's Data log. Must outlive the task. */
typedef struct {
    uint8_t  image;          /* PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS, for the file header */
    uint8_t  startup_state;  /* the image's STARTUP state: logging starts when the step leaves it */
    /* Each ring's storage (data_log_ring.h), payloads at most
       DATA_LOG_PAYLOAD_MAX. The step ring is required; the Event ring's
       entry is not read; any other left empty is a ring the image does
       not have. */
    data_log_ring_def_t rings[DATA_LOG_RING_COUNT];
    /* A Record written first in every file, sequence number 0, that the
       image's other Records need to be read (the VCU: its mailbox map,
       DATA_LOG_MAILBOXES). preamble_len 0: none. */
    uint8_t     preamble_type;
    const void *preamble;
    uint16_t    preamble_len;
} data_log_def_t;

/* The largest Record payload the logger takes from a ring: the BMS's
   snapshot Record (580 bytes) is the largest today. */
#define DATA_LOG_PAYLOAD_MAX 600u

/* Opens the rings and creates the status lock. Call from tasks_init()
   before the Control step and the Event writer are created. False if
   anything could not be made. */
bool data_log_init(const data_log_def_t *def);

/* The logger's entry point. argument is unused: the def is data_log_init()'s. */
void dataLogTask(void *argument);

/* The shell's `datalog` command: the card's state, the file open, Records
   written, dropped and not written. Name and output are provisional (#28). */
int data_log_cmd(int argc, char **argv);

#endif /* APP_COMMON_INC_DATA_LOG_H */
