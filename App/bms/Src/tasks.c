/* tasks.c — the one place BMS task creation happens. initTask calls
   tasks_init() once, then terminates; everything running after that
   point was created here.

   This is the empty BMS image: it boots, blinks the heartbeat LED and
   answers the common shell on USART1. The cell-monitor Sweep, the
   control step and the BMS pins come with later work. */

#include "tasks.h"

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
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
    /* print_init() must run before any task that might print is created.
       shell_init() arms the USART1 RX interrupt, so it must run before
       that interrupt can fire. The BMS has no shell commands of its own
       yet, so it never calls shell_register_commands(). */
    print_init();
    shell_init();

    create_task(printTask, NULL, "printTask",
                (osPriority_t)PRINT_TASK_PRIORITY, PRINT_TASK_STACK_SIZE);
    create_task(shellTask, NULL, "shellTask",
                (osPriority_t)SHELL_TASK_PRIORITY, SHELL_TASK_STACK_SIZE);
    create_task(heartbeatTask, NULL, "heartbeatTask",
                (osPriority_t)HEARTBEAT_TASK_PRIORITY, HEARTBEAT_TASK_STACK_SIZE);
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
