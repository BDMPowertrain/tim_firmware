/* shell.c — turns bytes arriving on USART1 into shell commands. The RX
   interrupt hands each byte off to shellTask through a stream buffer;
   shellTask assembles the bytes into a line (with local echo and
   backspace handling), then dispatches each finished line through the
   command table. All outgoing bytes, echo included, go through
   print.c's queue rather than touching the UART directly. */

#include "shell.h"

#include <stddef.h>
#include <string.h>

#include "cmsis_os2.h"

#include "control_bus.h"
#include "control_step.h"

#include "main.h"
#include "parameters.h"
#include "print.h"
#include "stats.h"
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
bool shell_init(void)
{
    shellRxStreamBuf = xStreamBufferCreate(SHELL_RX_BUF_SIZE, SHELL_RX_TRIGGER_LEVEL);
    if (shellRxStreamBuf == NULL) {
        return false;
    }

    return HAL_UART_Receive_IT(&huart1, &rxByte, 1) == HAL_OK;
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

/* The word `hang` needs after it, so a stray "hang" does not reset the
   board. */
#define HANG_CONFIRM_WORD "confirm"

static int cmd_help(int argc, char **argv);
static int cmd_tasks(int argc, char **argv);
static int cmd_reset(int argc, char **argv);
static int cmd_step(int argc, char **argv);
static int cmd_bus(int argc, char **argv);
static int cmd_hang(int argc, char **argv);

/* The commands every image has. Adding one is one row here plus one
   handler function — nothing else in shell.c changes. A command only one
   image has goes in that image's own table instead, see
   shell_register_commands(). */
static const shell_cmd_t commandTable[] = {
    { "help",      cmd_help,      "list commands" },
    { "tasks",     cmd_tasks,     "list tasks: state, priority, CPU share, stack free (each second)" },
    { "reset",     cmd_reset,     "software reset" },
    { "step",      cmd_step,      "control step: state, overruns, last and worst times" },
    { "bus",       cmd_bus,       "Control bus (CAN1): bus-off, error-passive, skipped, overflows, error counts" },
    { "param",     parameters_cmd, "param [dump] | set <name> <value> | save" },
    { "hang",      cmd_hang,      "hang " HANG_CONFIRM_WORD ": hang the control step, for a watchdog reset" },
};

#define COMMAND_TABLE_COUNT (sizeof(commandTable) / sizeof(commandTable[0]))

/* The running image's own commands, or none. Written once from
   tasks_init() before shellTask exists, only read after that. */
static const shell_cmd_t *imageCommands;
static size_t imageCommandCount;

void shell_register_commands(const shell_cmd_t *commands, size_t count)
{
    imageCommands = commands;
    imageCommandCount = count;
}

bool shell_wait_key(uint32_t timeout_ms)
{
    uint8_t key;

    return xStreamBufferReceive(shellRxStreamBuf, &key, 1, pdMS_TO_TICKS(timeout_ms)) > 0;
}

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (size_t i = 0; i < COMMAND_TABLE_COUNT; i++) {
        shell_printf("%-12s %s\r\n", commandTable[i].name, commandTable[i].help);
    }
    for (size_t i = 0; i < imageCommandCount; i++) {
        shell_printf("%-12s %s\r\n", imageCommands[i].name, imageCommands[i].help);
    }

    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    stats_print_tasks();

    return 0;
}

static int cmd_step(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    control_step_print();

    return 0;
}

static int cmd_bus(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    control_bus_print_status();

    return 0;
}

/* The bench's hang (issue 48): the control step spins inside its next
   step and the watchdog resets the MCU about 60 ms after that step's
   Kick. The osDelay() gives printTask time to send the message first:
   once the step spins, nothing below Realtime runs again. */
static int cmd_hang(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], HANG_CONFIRM_WORD) != 0) {
        shell_printf("usage: hang " HANG_CONFIRM_WORD "  (the watchdog resets the board)\r\n");
        return 0;
    }

    shell_printf("hanging the control step...\r\n");
    osDelay(50);
    control_step_request_hang();

    return 0;
}

/* NVIC_SystemReset() never returns, so the osDelay() is what actually
   matters here: shell_printf() only enqueues onto printQueue, it
   doesn't wait for printTask to drain it, so without a delay this
   message could be lost — the reset would fire before printTask ever
   gets scheduled to transmit it. 50 ms is generous headroom for a
   ~10-byte string at 115200 baud. */
static int cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    shell_printf("resetting...\r\n");
    osDelay(50);
    NVIC_SystemReset();

    return 0;
}

/* Splits line in place on spaces, looks argv[0] up in commandTable and
   then in the image's own commands, and calls its handler. An empty line or an unrecognised command both fail
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

    for (size_t i = 0; i < imageCommandCount; i++) {
        if (strcmp(argv[0], imageCommands[i].name) == 0) {
            imageCommands[i].handler(argc, argv);
            return;
        }
    }

    shell_printf("unknown command: %s\r\n", argv[0]);
}

/* 80 was fine before CAN: `can send1 <id> <hex>` needs room for a
   64-byte FD payload as hex (128 chars) plus the command/id/flags. */
#define SHELL_LINE_MAX 160u

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

    parameters_print_report();
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
