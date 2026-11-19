/* tasks.c — the one place BMS task creation happens. initTask calls
   tasks_init() once, then terminates; everything running after that
   point was created here.

   The BMS image so far: it boots, runs Housekeeping (the heartbeat LED
   and VBAT), answers the shell on USART1, runs its control step, which
   wakes the cell monitors, takes one Sweep every 20 ms and judges the
   BMS faults on it to drive BMS_OK, on the Control bus, and publishes its
   status on the DAQ bus. */

#include "tasks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "bms_adc.h"
#include "bms_daq.h"
#include "bms_log.h"
#include "bms_params.h"
#include "bms_shell.h"
#include "bms_step.h"
#include "can_control.h"
#include "can_daq.h"
#include "can_if.h"
#include "control_step.h"
#include "data_log.h"
#include "data_log_record.h"
#include "events.h"
#include "flash_bank2.h"
#include "housekeeping.h"
#include "main.h"
#include "parameters.h"
#include "print.h"
#include "record_ring.h"
#include "shell.h"
#include "watchdog.h"

/* tasks_init() runs on initTask's fixed 512-byte stack. Building each
   osThreadAttr_t inside its own call frees it before the next one is
   built, so the task count cannot overflow that stack — see the same
   helper in App/vcu/Src/tasks.c for the measurement behind this. */
static bool create_task(osThreadFunc_t func, void *argument, const char *name,
                        osPriority_t priority, uint32_t stack_size)
{
    const osThreadAttr_t attributes = {
        .name = name,
        .priority = priority,
        .stack_size = stack_size,
    };
    return osThreadNew(func, argument, &attributes) != NULL;
}

/* Read by the control step for as long as it runs. */
static control_step_args_t controlStepArgs;

/* Read by the Event writer for as long as it runs. */
static const events_task_args_t eventTaskArgs = {
    .image = PARAMS_IMAGE_BMS,
};

/* The Data log's step ring (ADR 0021, issue 69): 16 full snapshots deep.
   Each slot holds one step's Record, a pack Record or, every fifth Sweep,
   a snapshot (bms_log.h), so the ring holds 320 ms of 20 ms steps. About
   9.5 KB. */
#define STEP_RECORDS 16u

static uint32_t stepRecords[RECORD_RING_STORAGE_SIZE(STEP_RECORDS, BMS_LOG_SNAPSHOT_SIZE) /
                            sizeof(uint32_t)];

_Static_assert(BMS_LOG_SNAPSHOT_SIZE <= DATA_LOG_PAYLOAD_MAX,
               "the logger takes the BMS's snapshot Record");

/* Read by the Data log for as long as it runs. Logging starts when the
   step leaves STARTUP: BMS_OK has first risen, or a BMS fault came first. */
static const data_log_def_t dataLogDef = {
    .image = PARAMS_IMAGE_BMS,
    .startup_state = CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE,
    .rings = {
        [DATA_LOG_RING_STEP] = {stepRecords, sizeof stepRecords, BMS_LOG_SNAPSHOT_SIZE},
    },
};

/* VBAT, as on the VCU (#47): the newest averaged count from ADC1. */
static const housekeeping_def_t housekeepingDef = {
    .poll = bms_adc_poll_vbat,
};

/* The module summaries and cell dump (bms_daq.h), run by daqTask at each
   publish from its own copy of the published snapshot. Static: about
   550 bytes, daqTask's alone. */
static size_t build_daq_frames(uint32_t publish, can_frame_t *out, size_t max)
{
    static bms_pack_t pack;

    bms_step_read_pack(&pack);
    return bms_daq_build_frames(publish, &pack.sweep, pack.bleeding, out, max);
}

_Static_assert(BMS_DAQ_FRAMES_MAX <= CAN_IF_PUBLISH_FRAMES_MAX,
               "one publish's frames fit the DAQ send queue with the status frame");

/* What the DAQ publisher sends from the BMS (can/daq.dbc). */
static const can_if_publisher_t daqPublisher = {
    .status_frame_id = CAN_DAQ_BMS_DAQ_STATUS_FRAME_ID,
    .status_frame_len = CAN_DAQ_BMS_DAQ_STATUS_LENGTH,
    .build_status = bms_daq_build_status,
    .build_frames = build_daq_frames,
};

void tasks_init(void)
{
    /* Every queue and task is created once, here. A failure is remembered
       rather than fatal: the control step still runs, sends BMS_Status
       and keeps the BMS in STARTUP with BMS_OK low.

       print_init() must run before any task that might print is created.
       shell_init() arms the USART1 RX interrupt, so it must run before
       that interrupt can fire. The BMS's own commands are registered
       before shellTask is created; it never refuses `hang` or `crash`, so
       it registers no refusal hook. */
    bool ok = print_init();
    /* The Event queue before anything posts, and the flash lock before
       anything shares bank 2. */
    ok = events_init() && ok;
    ok = flash_bank2_init() && ok;
    /* The rings before the Control step and the Event writer push. */
    ok = data_log_init(&dataLogDef) && ok;
    /* Parameters before anything that reads them, read once (ADR 0015).
       The BMS accepts a save at any time, so it has no save hook. */
    ok = parameters_init(&bms_param_table, NULL) && ok;
    ok = shell_init() && ok;
    bms_shell_register();
    ok = can_if_init(&daqPublisher, parameters_value(BMS_PARAM_CAN2_TERM) != 0) && ok;
    ok = housekeeping_init() && ok;
    /* ADC1 before Housekeeping and the step read it; blocks 1-2 ms. */
    ok = bms_adc_start() && ok;
    ok = bms_step_init() && ok;
    /* Started in main(); a configuration that did not take is a start-up
       failure, so the BMS stays in STARTUP. */
    ok = watchdog_started() && ok;

    ok = create_task(printTask, NULL, "printTask",
                     (osPriority_t)PRINT_TASK_PRIORITY, PRINT_TASK_STACK_SIZE) && ok;
    ok = create_task(shellTask, NULL, "shellTask",
                     (osPriority_t)SHELL_TASK_PRIORITY, SHELL_TASK_STACK_SIZE) && ok;
    ok = create_task(housekeepingTask, (void *)&housekeepingDef, "housekeeping",
                     (osPriority_t)HOUSEKEEPING_TASK_PRIORITY, HOUSEKEEPING_TASK_STACK_SIZE) && ok;
    ok = create_task(canRxTask, NULL, "can2RxTask",
                     (osPriority_t)CAN_RX_TASK_PRIORITY, CAN_RX_TASK_STACK_SIZE) && ok;
    ok = create_task(daqTask, NULL, "daqTask",
                     (osPriority_t)DAQ_TASK_PRIORITY, DAQ_TASK_STACK_SIZE) && ok;
    ok = create_task(eventTask, (void *)&eventTaskArgs, "eventTask",
                     (osPriority_t)EVENT_TASK_PRIORITY, EVENT_TASK_STACK_SIZE) && ok;
    ok = create_task(dataLogTask, NULL, "dataLog",
                     (osPriority_t)DATA_LOG_TASK_PRIORITY, DATA_LOG_TASK_STACK_SIZE) && ok;

    /* The control step last, so it starts with everything else in place.
       Start-up Kick 2 of 2 (watchdog.h): from here the control step is the
       only Kicker, its first step one period away. */
    controlStepArgs.def = &bms_step_def;
    controlStepArgs.startup_ok = ok;
    watchdog_kick();
    (void)create_task(control_step_task, &controlStepArgs, "controlStep",
                      (osPriority_t)CONTROL_STEP_TASK_PRIORITY, CONTROL_STEP_TASK_STACK_SIZE);
}
