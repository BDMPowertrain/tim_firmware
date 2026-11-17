#ifndef APP_COMMON_INC_EVENTS_H
#define APP_COMMON_INC_EVENTS_H

/* events — the image's Event log (ADR 0014, spec #36): the one Event
   queue, the Event writer task that stores what it holds, and the shell's
   read-out. Storage is event_log.h logic over flash_bank2; the codes and
   their data are event_codes.h.

   Any task hands an Event to events_post(), which never waits: the queue
   is EVENTS_QUEUE_DEPTH deep, and a full queue drops the Event and counts
   it. The Control step posts this way too (ADR 0010: the queue is one of
   its three ways out), and so does start-up code before the writer runs.

   The writer (eventTask, BelowNormal) opens the log, writes the boot
   Event first (followed by the Crash record and the Breadcrumb when the
   last reset left them), then one Event per page repaired at open, then
   "not dual-bank" if the part is not, then whatever the queue holds,
   through the flood guard (event_flood.h). Events lost to a full queue or
   to the guard are written as one "N events dropped" Event.

   Without flash nothing is stored: the writer still runs the same
   sequence, "not dual-bank" included, and takes every Event from the
   queue, but stores none; the read-out says why the log is empty.

   Every Event, stored or not, is also copied to the Data log as a Record
   carrying the same 32 bytes (ADR 0021, data_log_ring.h).

   This file never checks which image it is built into. */

#include <stdbool.h>
#include <stdint.h>

#include "event_log.h"

#define EVENTS_QUEUE_DEPTH 16u

/* The argument of eventTask(). Must outlive the task. */
typedef struct {
    uint8_t image; /* PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS, for the boot Event */
    /* Run once on the writer's task, right after the log is opened and
       before anything is written to it, for a search of what an earlier
       start left (the VCU's Pedal check, ADR 0016). NULL for none. */
    void (*after_open)(void);
} events_task_args_t;

/* Creates the Event queue and the log's lock. Call first in tasks_init(),
   before anything posts. False if either could not be created. */
bool events_init(void);

/* Hands one Event to the writer, stamped with the uptime now. Never
   waits: false if the queue was full (or not created), and the Event is
   counted as dropped. Any task; not from an interrupt. */
bool events_post(uint16_t code, const uint8_t data[EVENT_DATA_SIZE]);

/* This start's boot number, once the writer has opened the log. False
   before then. Any task. */
bool events_boot_number(uint16_t *boot);

/* Searches the log from the newest Event to the oldest for one match()
   accepts (event_log_find()), under the log's lock. False before the
   writer has opened the log. Any task but the Control step, which takes
   no lock (ADR 0010). */
bool events_find(bool (*match)(const event_t *ev, const void *ctx), const void *ctx,
                 event_t *out);

/* Events waiting in the queue now. */
uint32_t events_queued(void);

/* The Event writer's entry point. argument is an events_task_args_t *. */
void eventTask(void *argument);

/* The shell's `events` command: events [n] prints the log's state and its
   newest n Events, oldest first. Output is provisional (#28). */
int events_cmd(int argc, char **argv);

#endif /* APP_COMMON_INC_EVENTS_H */
