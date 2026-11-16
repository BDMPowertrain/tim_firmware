/* tasks.c — the one place BMS task creation happens. initTask calls
   tasks_init() once, then terminates; everything running after that
   point was created here.

   The BMS image so far: it boots, blinks the heartbeat LED, answers the
   common shell on USART1, runs its control step on the Control bus and
   publishes its status on the DAQ bus. The cell-monitor Sweep and the BMS
   pins come with later work. */

#include "tasks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "bms_daq.h"
#include "bms_params.h"
#include "bms_step.h"
#include "can_daq.h"
#include "can_if.h"
#include "control_step.h"
#include "main.h"
#include "parameters.h"
#include "print.h"
#include "shell.h"

/* The board's "I'm alive" LED: 1 Hz, lowest priority of anything here. */
static void heartbeatTask(void *argument)
{
    (void)argument;

    for (;;) {
        HAL_GPIO_TogglePin(GPIO_HBLED_GPIO_Port, GPIO_HBLED_Pin);
        osDelay(500);
    }
}

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

/* What the DAQ publisher sends from the BMS (can/daq.dbc). */
static const can_if_publisher_t daqPublisher = {
    .status_frame_id = CAN_DAQ_BMS_DAQ_STATUS_FRAME_ID,
    .status_frame_len = CAN_DAQ_BMS_DAQ_STATUS_LENGTH,
    .build_status = bms_daq_build_status,
};

void tasks_init(void)
{
    /* Every queue and task is created once, here. A failure is remembered
       rather than fatal: the control step still runs, sends BMS_Status
       and keeps the BMS in STARTUP with BMS_OK low.

       print_init() must run before any task that might print is created.
       shell_init() arms the USART1 RX interrupt, so it must run before
       that interrupt can fire. The BMS has no shell commands of its own
       yet, so it never calls shell_register_commands(). */
    bool ok = print_init();
    /* Parameters before anything that reads them, read once (ADR 0015).
       The BMS accepts a save at any time, so it has no save hook. */
    ok = parameters_init(&bms_param_table, NULL) && ok;
    ok = shell_init() && ok;
    ok = can_if_init(&daqPublisher) && ok;
    ok = bms_step_init() && ok;

    ok = create_task(printTask, NULL, "printTask",
                     (osPriority_t)PRINT_TASK_PRIORITY, PRINT_TASK_STACK_SIZE) && ok;
    ok = create_task(shellTask, NULL, "shellTask",
                     (osPriority_t)SHELL_TASK_PRIORITY, SHELL_TASK_STACK_SIZE) && ok;
    ok = create_task(heartbeatTask, NULL, "heartbeatTask",
                     (osPriority_t)HEARTBEAT_TASK_PRIORITY, HEARTBEAT_TASK_STACK_SIZE) && ok;
    ok = create_task(canRxTask, NULL, "can2RxTask",
                     (osPriority_t)CAN_RX_TASK_PRIORITY, CAN_RX_TASK_STACK_SIZE) && ok;
    ok = create_task(daqTask, NULL, "daqTask",
                     (osPriority_t)DAQ_TASK_PRIORITY, DAQ_TASK_STACK_SIZE) && ok;

    /* The control step last, so it starts with everything else in place. */
    controlStepArgs.def = &bms_step_def;
    controlStepArgs.startup_ok = ok;
    (void)create_task(control_step_task, &controlStepArgs, "controlStep",
                      (osPriority_t)CONTROL_STEP_TASK_PRIORITY, CONTROL_STEP_TASK_STACK_SIZE);
}

/* Called by FreeRTOS the instant it detects a task has overrun its stack
   (configCHECK_FOR_STACK_OVERFLOW, FreeRTOSConfig.h). RTOS state is not
   trustworthy by now, so this only latches HBLED on solid and halts —
   visibly different from the 1 Hz heartbeat. Attach a debugger to read
   xTask and pcTaskName. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;

    __disable_irq();
    HAL_GPIO_WritePin(GPIO_HBLED_GPIO_Port, GPIO_HBLED_Pin, GPIO_PIN_RESET);
    for (;;) {
    }
}
