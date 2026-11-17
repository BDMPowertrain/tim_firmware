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
#include "housekeeping.h"
#include "imu.h"
#include "logger.h"
#include "main.h"
#include "parameters.h"
#include "print.h"
#include "shell.h"
#include "spi3_bus.h"
#include "vcu_adc.h"
#include "vcu_daq.h"
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
    /* Parameters before anything that reads them, read once (ADR 0015). */
    ok = parameters_init(&vcu_param_table, vcu_step_save_refusal) && ok;
    ok = shell_init() && ok;
    vcu_shell_register();
    ok = spi3_bus_init() && ok;
    ok = logger_init() && ok;
    ok = can_if_init(&daqPublisher) && ok;
    /* The analog inputs run from here on, the first scan discarded, so the
       control step's first read has samples (issue 55). */
    ok = vcu_adc_start() && ok;
    ok = housekeeping_init() && ok;
    ok = vcu_step_init() && ok;
    ok = control_step_init() && ok;
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
    ok = create_task(loggerTask, NULL, "loggerTask",
                     (osPriority_t)LOGGER_TASK_PRIORITY, LOGGER_TASK_STACK_SIZE) && ok;
    ok = create_task(canRxTask, NULL, "can2RxTask",
                     (osPriority_t)CAN_RX_TASK_PRIORITY, CAN_RX_TASK_STACK_SIZE) && ok;
    ok = create_task(daqTask, NULL, "daqTask",
                     (osPriority_t)DAQ_TASK_PRIORITY, DAQ_TASK_STACK_SIZE) && ok;

    /* The control step last, so it starts with everything else in place.
       Start-up Kick 2 of 2 (watchdog.h): from here the control step is the
       only Kicker, its first step one period away. */
    controlStepArgs.def = &vcu_step_def;
    controlStepArgs.startup_ok = ok;
    watchdog_kick();
    (void)create_task(control_step_task, &controlStepArgs, "controlStep",
                      (osPriority_t)CONTROL_STEP_TASK_PRIORITY, CONTROL_STEP_TASK_STACK_SIZE);
}
