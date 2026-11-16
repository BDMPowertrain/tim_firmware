/* print.c — owns USART1's transmit side. Every other App/ task that
   wants to put text on the shell goes through shell_printf()/
   shell_print_raw() here instead of touching the UART directly, so
   output from different tasks can never interleave into garbled bytes
   on the wire. printTask is the only code allowed to call
   HAL_UART_Transmit. */

#include "print.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "main.h"

extern UART_HandleTypeDef huart1;

#define PRINT_LINE_MAX  128u
#define PRINT_QUEUE_DEPTH 8u

/* One queue item: a null-terminated line, stored by value so the queue's
   copy-in/copy-out is all there is to worry about (no pointer, no
   lifetime to track). */
typedef struct {
    char text[PRINT_LINE_MAX];
} print_record_t;

/* Internal linkage on purpose: only this file may touch the queue.
   Everyone else goes through shell_printf()/shell_print_raw(). */
static osMessageQueueId_t printQueue;

/* Creates printQueue. Must run before any task that might call
   shell_printf()/shell_print_raw() is created — see tasks_init(). */
bool print_init(void)
{
    printQueue = osMessageQueueNew(PRINT_QUEUE_DEPTH, sizeof(print_record_t), NULL);
    return printQueue != NULL;
}

/* The only task allowed to touch huart1's TX side. Blocks waiting for a
   line, then blocking-transmits it. Runs forever. */
void printTask(void *argument)
{
    (void)argument;
    print_record_t record;

    for (;;) {
        if (osMessageQueueGet(printQueue, &record, NULL, osWaitForever) == osOK) {
            HAL_UART_Transmit(&huart1, (uint8_t *)record.text, strlen(record.text), HAL_MAX_DELAY);
        }
    }
}

/* Formats onto this task's own stack, then hands the whole line to the
   queue as one atomic item — safe to call from any task, no mutex
   needed, because there's no state shared between callers except the
   queue's own copy-in. */
void shell_printf(const char *fmt, ...)
{
    print_record_t record;
    va_list args;

    va_start(args, fmt);
    vsnprintf(record.text, sizeof(record.text), fmt, args);
    va_end(args);

    osMessageQueuePut(printQueue, &record, 0, osWaitForever);
}

/* Same destination as shell_printf(), for bytes that are already
   formatted (echo, backspace erase sequence) and don't need vsnprintf. */
void shell_print_raw(const char *buf, size_t len)
{
    print_record_t record;
    size_t copyLen = (len < sizeof(record.text) - 1) ? len : sizeof(record.text) - 1;

    memcpy(record.text, buf, copyLen);
    record.text[copyLen] = '\0';

    osMessageQueuePut(printQueue, &record, 0, osWaitForever);
}
