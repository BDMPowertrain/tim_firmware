#ifndef APP_INC_CONTROL_STEP_H
#define APP_INC_CONTROL_STEP_H

/* control_step — runs an image's Control step (ADR 0010): the one
   Realtime task, on fixed period boundaries by delay-until, taking no
   mutex. Each step runs the image's phases in order, times the step and
   each phase with the cycle counter, and publishes one snapshot that
   other tasks copy with interrupts masked.

   An Overrun is counted and the next step starts on the next period
   boundary, with no catch-up (step_timing.h). The image says what its
   phases are; beyond them the runner knows only the Control bus, which
   every image has, and whose report it publishes with the step so the
   step leaves through one snapshot (ADR 0010). This file never checks
   which image it is built into. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control_bus.h"
#include "step_timing.h"

/* What a phase is handed. now_ms and step_count are set before the first
   phase; state is the image's own state, written by its phases and
   published after the step. */
typedef struct {
    uint32_t now_ms;
    uint32_t step_count; /* 0 on the first step */
    bool     startup_ok; /* every task and queue was created */
    uint8_t  state;
} control_step_ctx_t;

typedef struct {
    const char *name;
    void (*run)(control_step_ctx_t *ctx);
} control_phase_t;

typedef struct {
    const char            *image;  /* "VCU" or "BMS", for the shell */
    uint32_t               period_ms;
    const control_phase_t *phases; /* in order; at most STEP_PHASE_MAX */
    size_t                 phase_count;
    const char          *(*state_name)(uint8_t state);
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
    uint32_t     overruns;   /* since power-on */
    step_stats_t stats;      /* last and worst since power-on, in us */
    control_bus_report_t bus; /* the Control bus as this step left it */
} control_snapshot_t;

/* The task entry point. argument is a control_step_args_t *. The first
   step starts one period after the task does. */
void control_step_task(void *argument);

/* Copies the latest published snapshot, with interrupts masked. Any task. */
void control_step_read(control_snapshot_t *out);

/* The shell's `step` command: prints the snapshot. */
void control_step_print(void);

#endif /* APP_INC_CONTROL_STEP_H */
