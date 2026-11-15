#ifndef APP_INC_SHELL_H
#define APP_INC_SHELL_H

#include <stdint.h>

#include "FreeRTOS.h"

/* Assembles bytes from shellRxStreamBuf into lines with local echo and
   backspace handling, tokenizes each finished line, and dispatches it
   through the command table. */
void shellTask(void *argument);

/* Creates shellRxStreamBuf and arms the first USART1 RX interrupt. Must
   run before any RX interrupt can occur — see tasks_init(). */
void shell_init(void);

/* Called from HAL_UART_RxCpltCallback with the one byte just received;
   pushes it into shellRxStreamBuf. Nothing else runs at interrupt level. */
void shell_rx_isr_byte(uint8_t byte, BaseType_t *pxHigherPriorityTaskWoken);

#endif /* APP_INC_SHELL_H */
