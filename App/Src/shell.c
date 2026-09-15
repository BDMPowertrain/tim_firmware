/* shell.c — turns bytes arriving on USART1 into shell commands. The RX
   interrupt hands each byte off to shellTask through a stream buffer;
   shellTask assembles the bytes into a line (with local echo and
   backspace handling), then dispatches each finished line through the
   command table. All outgoing bytes, echo included, go through
   print.c's queue rather than touching the UART directly. */

#include "shell.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "print.h"
#include "stream_buffer.h"

extern UART_HandleTypeDef huart1;

#define SHELL_RX_BUF_SIZE      64u
#define SHELL_RX_TRIGGER_LEVEL 1u

/* Internal linkage: only this file's ISR-side and task-side code touch
   the RX stream buffer directly. */
static StreamBufferHandle_t shellRxStreamBuf;

/* Storage for HAL_UART_Receive_IT's one-byte-at-a-time reception. Must
   stay alive for as long as reception can be pending, so it's static,
   not a stack variable. */
static uint8_t rxByte;

/* Creates shellRxStreamBuf and arms the first RX interrupt. Must run
   before any RX interrupt can occur, since the callback below assumes
   shellRxStreamBuf already exists. */
void shell_init(void)
{
    shellRxStreamBuf = xStreamBufferCreate(SHELL_RX_BUF_SIZE, SHELL_RX_TRIGGER_LEVEL);
    if (shellRxStreamBuf == NULL) {
        Error_Handler();
    }

    if (HAL_UART_Receive_IT(&huart1, &rxByte, 1) != HAL_OK) {
        Error_Handler();
    }
}

/* ISR-level work: push the one received byte into the stream buffer.
   No parsing, no echo, nothing blocking happens here. */
void shell_rx_isr_byte(uint8_t byte, BaseType_t *pxHigherPriorityTaskWoken)
{
    xStreamBufferSendFromISR(shellRxStreamBuf, &byte, 1, pxHigherPriorityTaskWoken);
}

/* HAL's weak RX-complete callback, overridden here (USE_HAL_UART_REGISTER_CALLBACKS
   is 0 in this project, so a strong definition of the classic weak function is
   the correct override mechanism). Runs in USART1's ISR context. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (huart->Instance == USART1) {
        shell_rx_isr_byte(rxByte, &xHigherPriorityTaskWoken);
        HAL_UART_Receive_IT(&huart1, &rxByte, 1);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

#define SHELL_MAX_ARGS 8u

typedef int (*shell_cmd_fn_t)(int argc, char **argv);

typedef struct {
    const char     *name;
    shell_cmd_fn_t  handler;
    const char     *help;
} shell_cmd_t;

static int cmd_help(int argc, char **argv);

/* Adding a command is one row here plus one handler function — nothing
   else in shell.c changes. */
static const shell_cmd_t commandTable[] = {
    { "help", cmd_help, "list commands" },
};

#define COMMAND_TABLE_COUNT (sizeof(commandTable) / sizeof(commandTable[0]))

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (size_t i = 0; i < COMMAND_TABLE_COUNT; i++) {
        shell_printf("%-12s %s\r\n", commandTable[i].name, commandTable[i].help);
    }

    return 0;
}

/* Splits line in place on spaces, looks argv[0] up in commandTable, and
   calls its handler. An empty line or an unrecognised command both fail
   safely — nothing crashes, the user just sees nothing or an error. */
static void shell_dispatch(char *line)
{
    char *argv[SHELL_MAX_ARGS];
    int argc = 0;
    char *token = strtok(line, " ");

    while (token != NULL && argc < (int)SHELL_MAX_ARGS) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    if (argc == 0) {
        return;
    }

    for (size_t i = 0; i < COMMAND_TABLE_COUNT; i++) {
        if (strcmp(argv[0], commandTable[i].name) == 0) {
            commandTable[i].handler(argc, argv);
            return;
        }
    }

    shell_printf("unknown command: %s\r\n", argv[0]);
}

#define SHELL_LINE_MAX 80u

/* Reads bytes out of shellRxStreamBuf, assembles them into a line with
   local echo and backspace handling, and dispatches each finished line.
   Everything it sends back out (echo included) goes through
   shell_print_raw()/shell_printf() — this task never touches huart1
   directly, printTask owns that. */
void shellTask(void *argument)
{
    (void)argument;
    char line[SHELL_LINE_MAX];
    size_t lineLen = 0;
    uint8_t ch;

    shell_printf("> ");

    for (;;) {
        if (xStreamBufferReceive(shellRxStreamBuf, &ch, 1, portMAX_DELAY) > 0) {
            if (ch == '\r' || ch == '\n') {
                shell_print_raw("\r\n", 2);
                line[lineLen] = '\0';
                shell_dispatch(line);
                lineLen = 0;
                shell_printf("> ");
            } else if (ch == 0x08 || ch == 0x7F) {
                if (lineLen > 0) {
                    lineLen--;
                    shell_print_raw("\b \b", 3);
                }
            } else if (lineLen < SHELL_LINE_MAX - 1) {
                line[lineLen++] = (char)ch;
                shell_print_raw((char *)&ch, 1);
            }
        }
    }
}
