/* tasks.c — the one place task creation happens. initTask calls
   tasks_init() once, then terminates; everything running after that
   point was created here. See Documentation/tim-rtos-architecture.md
   section 3 for why this replaces CubeMX's Tasks and Queues tab. */

#include "tasks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "can_if.h"
#include "imu.h"
#include "logger.h"
#include "main.h"
#include "print.h"
#include "shell.h"
#include "spi3_bus.h"

/* Ladder exercise 1: two tasks toggling different GPIOs at different
   rates and priorities, so preemption is visible on a scope rather than
   just read about. heartbeatTask is slow and low-priority (the board's
   "I'm alive" LED); blinkTask is fast and Normal-priority, so it
   preempts heartbeatTask every time both are runnable at once. */
static void heartbeatTask(void *argument)
{
    (void)argument;

    for (;;) {
        HAL_GPIO_TogglePin(GPIO_HBLED_GPIO_Port, GPIO_HBLED_Pin);
        osDelay(500);
    }
}

static void blinkTask(void *argument)
{
    (void)argument;

    for (;;) {
        HAL_GPIO_TogglePin(GPIO_1_GPIO_Port, GPIO_1_Pin);
        osDelay(5);
    }
}

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
static void create_task(osThreadFunc_t func, void *argument, const char *name,
                         osPriority_t priority, uint32_t stack_size)
{
    const osThreadAttr_t attributes = {
        .name = name,
        .priority = priority,
        .stack_size = stack_size,
    };
    osThreadNew(func, argument, &attributes);
}

void tasks_init(void)
{
    /* print_init() must run before any task that might call shell_printf()/
       shell_print_raw() is created — printTask itself included, since it's
       the one draining printQueue. shell_init() arms the USART1 RX
       interrupt, so it must run before that interrupt can fire; creating
       shellTask afterward is what actually drains what it collects. */
    print_init();
    shell_init();
    spi3_bus_init();
    logger_init();
    can_if_init();

    create_task(printTask, NULL, "printTask",
                (osPriority_t)PRINT_TASK_PRIORITY, PRINT_TASK_STACK_SIZE);
    create_task(shellTask, NULL, "shellTask",
                (osPriority_t)SHELL_TASK_PRIORITY, SHELL_TASK_STACK_SIZE);
    create_task(heartbeatTask, NULL, "heartbeatTask",
                (osPriority_t)HEARTBEAT_TASK_PRIORITY, HEARTBEAT_TASK_STACK_SIZE);
    create_task(blinkTask, NULL, "blinkTask",
                (osPriority_t)BLINK_TASK_PRIORITY, BLINK_TASK_STACK_SIZE);
    create_task(imuTask, NULL, "imuTask",
                (osPriority_t)IMU_TASK_PRIORITY, IMU_TASK_STACK_SIZE);
    create_task(loggerTask, NULL, "loggerTask",
                (osPriority_t)LOGGER_TASK_PRIORITY, LOGGER_TASK_STACK_SIZE);

    /* canRxTask/canTxTask are each one function reused for both buses —
       argument is the can_bus_t value itself (not a pointer), see
       can_if.h. */
    create_task(canRxTask, (void *)CAN_BUS_1, "can1RxTask",
                (osPriority_t)CAN_RX_TASK_PRIORITY, CAN_RX_TASK_STACK_SIZE);
    create_task(canTxTask, (void *)CAN_BUS_1, "can1TxTask",
                (osPriority_t)CAN_TX_TASK_PRIORITY, CAN_TX_TASK_STACK_SIZE);
    create_task(canRxTask, (void *)CAN_BUS_2, "can2RxTask",
                (osPriority_t)CAN_RX_TASK_PRIORITY, CAN_RX_TASK_STACK_SIZE);
    create_task(canTxTask, (void *)CAN_BUS_2, "can2TxTask",
                (osPriority_t)CAN_TX_TASK_PRIORITY, CAN_TX_TASK_STACK_SIZE);
}

/* Called by FreeRTOS the instant it detects a task has overrun its
   stack (configCHECK_FOR_STACK_OVERFLOW, FreeRTOSConfig.h). By this
   point pcTaskName and surrounding memory may already be corrupted, and
   the offending task's own stack is not trustworthy — so this does
   nothing but latch HBLED on solid and halt, deliberately not using
   shell_printf()/osDelay()/anything that assumes RTOS state is sane.
   Solid-on is visibly different from heartbeatTask's normal 1 Hz blink,
   which stops the instant this fires. Attach a debugger to read xTask
   and pcTaskName from here to find out which task overran. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;

    __disable_irq();
    HAL_GPIO_WritePin(GPIO_HBLED_GPIO_Port, GPIO_HBLED_Pin, GPIO_PIN_RESET);
    for (;;) {
    }
}
