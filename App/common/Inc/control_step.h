#ifndef APP_INC_CONTROL_STEP_H
#define APP_INC_CONTROL_STEP_H

/* control_step — runs an image's Control step (ADR 0010): the one
   Realtime task, on fixed period boundaries by delay-until, taking no
   mutex. Each step runs the image's phases in order, times the step and
   each phase with the cycle counter, and publishes one snapshot that
   other tasks copy with interrupts masked.

   An Overrun is counted and the next step starts on the next period
   boundary, with no catch-up (step_timing.h). Each Overrun posts an
   overrun Event (overrun_watch.h, event_codes.h) to the 16-deep Event
   queue with zero timeout (events.h); a full queue drops it and counts. Three Overruns within one
   second set timing_fault for the next step's phases.

   Where the step is goes to the Breadcrumb in the backup registers
   (reset_info.h) at the start of each step and of each phase, so a
   watchdog reset can say what the step was doing (ADR 0013).

   The step is the only Kicker of the watchdog once it runs (ADR 0013):
   one call, at the start of each step, made only if the previous step ran
   every phase in order (kick_gate.h). The first step Kicks
   unconditionally.

   The image says what its phases are; beyond them the runner knows only
   the Control bus, which every image has, and whose report it publishes
   with the step, so the step leaves through one snapshot and the Event
   queue (ADR 0010). This file never checks
   which image it is built into. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control_bus.h"
#include "overrun_watch.h"
#include "step_timing.h"

/* What a phase is handed. now_ms, step_count, background_stalled and
   timing_fault are set before the first phase; state and fault_active are
   the image's own, written by its phases and published after the step. */
typedef struct {
    uint32_t now_ms;
    uint32_t step_count; /* 0 on the first step */
    bool     startup_ok; /* every task and queue was created */
    /* Housekeeping has not run for 1 s (stall_watch.h): a warning only. */
    bool     background_stalled;
    /* The previous step was the third Overrun within one second
       (overrun_watch.h). True for this one step; the image's logic decides
       what to latch: a BMS fault on the BMS, a Torque cut on the VCU. */
    bool     timing_fault;
    uint8_t  state;
    /* Any fault is active: the heartbeat LED blinks at 5 Hz (heartbeat.h). */
    bool     fault_active;
} control_step_ctx_t;

typedef struct {
    const char *name;
    void (*run)(control_step_ctx_t *ctx);
} control_phase_t;

/* Room in the published snapshot for the image's own data, such as the
   VCU's sensor readings. */
#define CONTROL_IMAGE_DATA_MAX 64u

typedef struct {
    const char            *image;  /* "VCU" or "BMS", for the shell */
    uint32_t               period_ms;
    const control_phase_t *phases; /* in order; at most STEP_PHASE_MAX */
    size_t                 phase_count;
    const char          *(*state_name)(uint8_t state);
    /* The image's own data, written by its phases and published with the
       step (at most CONTROL_IMAGE_DATA_MAX bytes; more is cut). The image
       reads it back out of control_snapshot_t.image_data. NULL for none. */
    const void            *image_data;
    size_t                 image_data_size;
} control_step_def_t;

/* The argument of control_step_task(). Must outlive the task. */
typedef struct {
    const control_step_def_t *def;
    bool                      startup_ok;
} control_step_args_t;

/* The published snapshot, written once per step. */
typedef struct {
    bool         running;    /* at least one step has run */
    uint32_t     step_count; /* steps run */
    uint8_t      state;
    bool         startup_ok;
    bool         fault_active;
    bool         background_stalled;
    uint32_t     overruns;   /* since power-on */
    uint32_t     timing_faults;   /* times three Overruns fell in one second */
    uint32_t     kicks_withheld;  /* steps that found the previous one incomplete */
    uint32_t     events_dropped;  /* overrun Events that found the Event queue full */
    step_stats_t stats;      /* last and worst since power-on, in us */
    control_bus_report_t bus; /* the Control bus as this step left it */
    union {
        uint8_t  bytes[CONTROL_IMAGE_DATA_MAX];
        uint32_t align;
    } image_data;             /* def->image_data as this step left it */
} control_snapshot_t;

/* The task entry point. argument is a control_step_args_t *. The first
   step starts one period after the task does. */
void control_step_task(void *argument);

/* The bench's hang (issue 48): the next step Kicks, then spins for ever
   at Realtime priority, so the watchdog resets the MCU one timeout after
   that Kick. Any task; the shell's `hang` asks for a confirmation word
   first. Exists in every build: the image measured is the image raced. */
void control_step_request_hang(void);

/* The name of the image's phase at index, for the Breadcrumb's readout.
   NULL before the task runs, or past the last phase. Any task. */
const char *control_step_phase_name(size_t index);

/* Copies the latest published snapshot, with interrupts masked. Any task. */
void control_step_read(control_snapshot_t *out);

/* The shell's `step` command: prints the snapshot. */
void control_step_print(void);

#endif /* APP_INC_CONTROL_STEP_H */
