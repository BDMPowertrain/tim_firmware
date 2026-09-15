/* tasks.c — the one place task creation happens. initTask calls
   tasks_init() once, then terminates; everything running after that
   point was created here. See Documentation/tim-rtos-architecture.md
   section 3 for why this replaces CubeMX's Tasks and Queues tab. */

#include "tasks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "imu.h"
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

    const osThreadAttr_t printTask_attributes = {
        .name = "printTask",
        .priority = (osPriority_t)PRINT_TASK_PRIORITY,
        .stack_size = PRINT_TASK_STACK_SIZE,
    };
    osThreadNew(printTask, NULL, &printTask_attributes);

    const osThreadAttr_t shellTask_attributes = {
        .name = "shellTask",
        .priority = (osPriority_t)SHELL_TASK_PRIORITY,
        .stack_size = SHELL_TASK_STACK_SIZE,
    };
    osThreadNew(shellTask, NULL, &shellTask_attributes);

    const osThreadAttr_t heartbeatTask_attributes = {
        .name = "heartbeatTask",
        .priority = (osPriority_t)HEARTBEAT_TASK_PRIORITY,
        .stack_size = HEARTBEAT_TASK_STACK_SIZE,
    };
    osThreadNew(heartbeatTask, NULL, &heartbeatTask_attributes);

    const osThreadAttr_t blinkTask_attributes = {
        .name = "blinkTask",
        .priority = (osPriority_t)BLINK_TASK_PRIORITY,
        .stack_size = BLINK_TASK_STACK_SIZE,
    };
    osThreadNew(blinkTask, NULL, &blinkTask_attributes);

    const osThreadAttr_t imuTask_attributes = {
        .name = "imuTask",
        .priority = (osPriority_t)IMU_TASK_PRIORITY,
        .stack_size = IMU_TASK_STACK_SIZE,
    };
    osThreadNew(imuTask, NULL, &imuTask_attributes);
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
