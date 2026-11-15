#ifndef APP_INC_SHELL_H
#define APP_INC_SHELL_H

#include <stdbool.h>
#include <stddef.h>
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

/* One shell command: the word typed, the function that runs it and the
   line `help` prints for it. */
typedef int (*shell_cmd_fn_t)(int argc, char **argv);

typedef struct {
    const char     *name;
    shell_cmd_fn_t  handler;
    const char     *help;
} shell_cmd_t;

/* Adds the running image's own commands after the common ones. The shell
   keeps the pointer, so the table must stay alive (make it static const).
   Call once, before shellTask is created; a second call replaces the
   first. An image with no commands of its own never calls this. */
void shell_register_commands(const shell_cmd_t *commands, size_t count);

/* For a command that streams until a key is pressed: blocks the calling
   command for up to timeout_ms and returns true if a key arrived. The key
   is consumed, so it is neither echoed nor run as a command. Only call
   from a command handler, i.e. on shellTask. */
bool shell_wait_key(uint32_t timeout_ms);

#endif /* APP_INC_SHELL_H */
