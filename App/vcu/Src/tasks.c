/* tasks.c — the one place task creation happens. initTask calls
   tasks_init() once, then terminates; everything running after that
   point was created here. See Documentation/tim-rtos-architecture.md
   section 3 for why this replaces CubeMX's Tasks and Queues tab. */

#include "tasks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "can_daq.h"
#include "can_if.h"
#include "control_step.h"
#include "data_log.h"
#include "data_log_record.h"
#include "events.h"
#include "flash_bank2.h"
#include "housekeeping.h"
#include "imu.h"
#include "main.h"
#include "parameters.h"
#include "print.h"
#include "record_ring.h"
#include "shell.h"
#include "spi3_bus.h"
#include "vcu_adc.h"
#include "vcu_control.h"
#include "vcu_daq.h"
#include "vcu_log.h"
#include "vcu_params.h"
#include "vcu_shell.h"
#include "vcu_step.h"
#include "watchdog.h"

/* initTask (the one CubeMX-generated task, see Core/Src/main.c) has a
   fixed 512-byte stack that isn't one of tasks.h's tunable *_STACK_SIZE
   defines — it's set in CubeMX's FreeRTOS Tasks tab, outside any USER
   CODE marker. tasks_init() runs entirely on THAT stack, not the new
   task's own. At -O0 (this project's debug build), the compiler does
   NOT reuse stack slots across sequential locals in one function scope
   — every const osThreadAttr_t declared directly in tasks_init() stays
   resident for the whole function, all of them simultaneously. Adding
   CAN's 4 tasks brought the count to 10 (sizeof(osThreadAttr_t) == 36
   bytes -> 360 bytes of just these structs against a 512-byte budget)
   and overflowed initTask's stack — this helper exists specifically to
   make that impossible regardless of task count: each call is a real
   function boundary, so its local attr struct's frame is freed the
   moment that call returns, before the next one is even built. */
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
    .image = PARAMS_IMAGE_VCU,
    .after_open = vcu_step_find_pedal_check, /* ADR 0016 */
};

/* The Data log's rings (ADR 0021, issue 61). The logger empties them every
   20 ms; the depth covers a card that stalls for a few hundred ms.
     step    64 full step Records (vcu_log.h), 640 ms of steps.
     imu     64 accel and gyro pairs, about 615 ms at 104 Hz.
     daq rx  32 DAQ bus frames heard, daq tx 16 sent: the DAQ bus carries
             nothing safety-related, so these drop first under a burst.

   If the bench measurements of ADR 0021 fail (idle below 50% with logging
   on, or a worse step time), the retreat order is:
     1. drop the DAQ bus recording: leave the daq rx and daq tx rings out
        of dataLogDef below, and can_if pushes nothing;
     2. lower the IMU rate: the batch rate in imu.c (FIFO_CTRL3, and
        VCU_IMU_PERIOD_US in vcu_imu_log.h with it);
   the step Records go last. */
#define STEP_RECORDS   64u
#define IMU_RECORDS    64u
#define DAQ_RX_RECORDS 32u
#define DAQ_TX_RECORDS 16u

static uint32_t stepRecords[RECORD_RING_STORAGE_SIZE(STEP_RECORDS, VCU_LOG_STEP_SIZE) /
                            sizeof(uint32_t)];
static uint32_t imuRecords[RECORD_RING_STORAGE_SIZE(IMU_RECORDS, DATA_LOG_IMU_SIZE) /
                           sizeof(uint32_t)];
static uint32_t daqRxRecords[RECORD_RING_STORAGE_SIZE(DAQ_RX_RECORDS, DATA_LOG_DAQ_SIZE_MAX) /
                             sizeof(uint32_t)];
static uint32_t daqTxRecords[RECORD_RING_STORAGE_SIZE(DAQ_TX_RECORDS, DATA_LOG_DAQ_SIZE_MAX) /
                             sizeof(uint32_t)];

_Static_assert(VCU_LOG_STEP_SIZE <= DATA_LOG_PAYLOAD_MAX && VCU_LOG_MAP_SIZE <= DATA_LOG_PAYLOAD_MAX,
               "the logger takes the VCU's step Record and map");

/* The step Record's mailboxes by frame ID, first in every file: filled in
   tasks_init(). */
static uint8_t mailboxMap[VCU_LOG_MAP_SIZE];

/* Read by the Data log for as long as it runs. */
static const data_log_def_t dataLogDef = {
    .image = PARAMS_IMAGE_VCU,
    .startup_state = VCU_STATE_STARTUP,
    .rings = {
        [DATA_LOG_RING_STEP] = {stepRecords, sizeof stepRecords, VCU_LOG_STEP_SIZE},
        [DATA_LOG_RING_IMU] = {imuRecords, sizeof imuRecords, DATA_LOG_IMU_SIZE},
        [DATA_LOG_RING_DAQ_RX] = {daqRxRecords, sizeof daqRxRecords, DATA_LOG_DAQ_SIZE_MAX},
        [DATA_LOG_RING_DAQ_TX] = {daqTxRecords, sizeof daqTxRecords, DATA_LOG_DAQ_SIZE_MAX},
    },
    .preamble_type = DATA_LOG_MAILBOXES,
    .preamble = mailboxMap,
    .preamble_len = sizeof mailboxMap,
};

/* The VCU's own Housekeeping work: VBAT, from the ADC1 scan. */
static const housekeeping_def_t housekeepingDef = {
    .poll = vcu_adc_poll_vbat,
};

/* What the DAQ publisher sends from the VCU (can/daq.dbc). */
static const can_if_publisher_t daqPublisher = {
    .status_frame_id = CAN_DAQ_VCU_DAQ_STATUS_FRAME_ID,
    .status_frame_len = CAN_DAQ_VCU_DAQ_STATUS_LENGTH,
    .build_status = vcu_daq_build_status,
};

void tasks_init(void)
{
    /* Every queue, mutex and task is created once, here. A failure is
       remembered rather than fatal: the control step still runs, sends
       its status frame and keeps the VCU in STARTUP.

       print_init() must run before any task that might call shell_printf()/
       shell_print_raw() is created — printTask itself included, since it's
       the one draining printQueue. shell_init() arms the USART1 RX
       interrupt, so it must run before that interrupt can fire; creating
       shellTask afterward is what actually drains what it collects. */
    bool ok = print_init();
    /* The Event queue before anything posts, and the flash lock before
       anything shares bank 2. */
    ok = events_init() && ok;
    ok = flash_bank2_init() && ok;
    /* Parameters before anything that reads them, read once (ADR 0015). */
    ok = parameters_init(&vcu_param_table, vcu_step_save_refusal) && ok;
    ok = shell_init() && ok;
    vcu_shell_register();
    ok = spi3_bus_init() && ok;
    /* The rings before the Control step, the Event writer, the IMU task and
       the DAQ bus tasks push. */
    vcu_log_map_encode(mailboxMap);
    ok = data_log_init(&dataLogDef) && ok;
    ok = can_if_init(&daqPublisher, parameters_value(VCU_PARAM_CAN2_TERM) != 0) && ok;
    /* The analog inputs run from here on, the first scan discarded, so the
       control step's first read has samples (issue 55). */
    ok = vcu_adc_start() && ok;
    ok = housekeeping_init() && ok;
    ok = vcu_step_init() && ok;
    /* Started in main(); a configuration that did not take is a start-up
       failure, so the image stays in STARTUP. */
    ok = watchdog_started() && ok;

    ok = create_task(printTask, NULL, "printTask",
                     (osPriority_t)PRINT_TASK_PRIORITY, PRINT_TASK_STACK_SIZE) && ok;
    ok = create_task(shellTask, NULL, "shellTask",
                     (osPriority_t)SHELL_TASK_PRIORITY, SHELL_TASK_STACK_SIZE) && ok;
    ok = create_task(housekeepingTask, (void *)&housekeepingDef, "housekeeping",
                     (osPriority_t)HOUSEKEEPING_TASK_PRIORITY, HOUSEKEEPING_TASK_STACK_SIZE) && ok;
    ok = create_task(imuTask, NULL, "imuTask",
                     (osPriority_t)IMU_TASK_PRIORITY, IMU_TASK_STACK_SIZE) && ok;
    ok = create_task(dataLogTask, NULL, "dataLog",
                     (osPriority_t)DATA_LOG_TASK_PRIORITY, DATA_LOG_TASK_STACK_SIZE) && ok;
    ok = create_task(canRxTask, NULL, "can2RxTask",
                     (osPriority_t)CAN_RX_TASK_PRIORITY, CAN_RX_TASK_STACK_SIZE) && ok;
    ok = create_task(daqTask, NULL, "daqTask",
                     (osPriority_t)DAQ_TASK_PRIORITY, DAQ_TASK_STACK_SIZE) && ok;
    ok = create_task(eventTask, (void *)&eventTaskArgs, "eventTask",
                     (osPriority_t)EVENT_TASK_PRIORITY, EVENT_TASK_STACK_SIZE) && ok;

    /* The control step last, so it starts with everything else in place.
       Start-up Kick 2 of 2 (watchdog.h): from here the control step is the
       only Kicker, its first step one period away. */
    controlStepArgs.def = &vcu_step_def;
    controlStepArgs.startup_ok = ok;
    watchdog_kick();
    (void)create_task(control_step_task, &controlStepArgs, "controlStep",
                      (osPriority_t)CONTROL_STEP_TASK_PRIORITY, CONTROL_STEP_TASK_STACK_SIZE);
}
