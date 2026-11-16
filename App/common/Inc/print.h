#ifndef APP_INC_PRINT_H
#define APP_INC_PRINT_H

#include <stdbool.h>
#include <stddef.h>

/* Creates printQueue. Must run before any task that might call
   shell_printf()/shell_print_raw() is created. False if it could not. */
bool print_init(void);

/* Owns huart1's TX side exclusively: blocks on printQueue, blocking-
   transmits each record it receives. No other code may call
   HAL_UART_Transmit on huart1. */
void printTask(void *argument);

/* vsnprintf's into a stack buffer, then posts it to printQueue as one
   atomic record. The only formatted-output entry point in App/. */
void shell_printf(const char *fmt, ...);

/* Posts pre-formatted bytes (echo, backspace erase sequence) to
   printQueue without the vsnprintf step. */
void shell_print_raw(const char *buf, size_t len);

#endif /* APP_INC_PRINT_H */
