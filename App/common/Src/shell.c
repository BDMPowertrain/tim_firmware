/* shell.c — turns bytes arriving on USART1 into shell commands. The RX
   interrupt hands each byte off to shellTask through a stream buffer;
   shellTask assembles the bytes into a line (with local echo and
   backspace handling), then dispatches each finished line through the
   command table. All outgoing bytes, echo included, go through
   print.c's queue rather than touching the UART directly. */

#include "shell.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include "can_if.h"
#include "imu.h"
#include "logger.h"
#include "main.h"
#include "print.h"
#include "stats.h"
#include "stream_buffer.h"
#include "vbat.h"

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
static int cmd_tasks(int argc, char **argv);
static int cmd_imu(int argc, char **argv);
static int cmd_imustream(int argc, char **argv);
static int cmd_reset(int argc, char **argv);
static int cmd_log(int argc, char **argv);
static int cmd_can(int argc, char **argv);
static int cmd_vbat(int argc, char **argv);

/* Adding a command is one row here plus one handler function — nothing
   else in shell.c changes. */
static const shell_cmd_t commandTable[] = {
    { "help",      cmd_help,      "list commands" },
    { "tasks",     cmd_tasks,     "list tasks: state, priority, stack free" },
    { "imu",       cmd_imu,       "latest accel/gyro sample" },
    { "imustream", cmd_imustream, "live accel/gyro stream, any key to stop" },
    { "reset",     cmd_reset,     "software reset" },
    { "log",       cmd_log,       "log start|stop|status|list|dump <file>" },
    { "can",       cmd_can,       "can status|stby<1|2>|term<1|2> on|off|send<1|2> <id> <hex> [fd] [brs]|stream<1|2>" },
    { "vbat",      cmd_vbat,      "battery voltage" },
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

static int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    stats_print_tasks();

    return 0;
}

/* mg/mdps conversion done in integer math, not float: this project
   links --specs=nano.specs without -u _printf_float (cmake/gcc-arm-none
   -eabi.cmake), so newlib-nano's vsnprintf silently mis-renders %f.
   IMU_GYRO_MDPS_PER_LSB (70) is already a whole number so raw*70 is
   exact; IMU_ACCEL_MG_PER_LSB (0.244) is scaled by 1000 and truncated,
   fine for a glance-at-it shell readout, not for logged data. */
static int cmd_imu(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    imu_sample_t accel;
    imu_sample_t gyro;

    if (imu_get_latest_accel(&accel)) {
        shell_printf("accel  raw x=%6d y=%6d z=%6d  mg x=%6ld y=%6ld z=%6ld\r\n",
                     accel.x, accel.y, accel.z,
                     (long)accel.x * 244 / 1000, (long)accel.y * 244 / 1000, (long)accel.z * 244 / 1000);
    } else {
        shell_printf("accel  no sample yet\r\n");
    }

    if (imu_get_latest_gyro(&gyro)) {
        shell_printf("gyro   raw x=%6d y=%6d z=%6d  mdps x=%6ld y=%6ld z=%6ld\r\n",
                     gyro.x, gyro.y, gyro.z,
                     (long)gyro.x * 70, (long)gyro.y * 70, (long)gyro.z * 70);
    } else {
        shell_printf("gyro   no sample yet\r\n");
    }

    uint16_t pending;
    bool watermarkFlag;
    if (imu_fifo_status(&pending, &watermarkFlag)) {
        shell_printf("fifo   pending=%u watermark_flag=%u\r\n", pending, (unsigned)watermarkFlag);
    }

    return 0;
}

/* Prints the cached accel/gyro sample roughly once per imuTask drain
   cycle (~100 ms, set by IMU_FIFO_WATERMARK in imu.c) until any key is
   pressed. xStreamBufferReceive()'s timeout does double duty as both
   the print-rate timer and the stop-key check, so there's no separate
   polling loop and no busy-waiting — this task is fully blocked between
   prints. The byte that stops the stream is consumed here and never
   reaches shell_dispatch(), so it doesn't echo or get treated as a
   command; the very next line is a fresh `> ` prompt. Raw counts only
   (not mg/mdps) so the stream stays compact enough to actually watch
   scroll by. */
static int cmd_imustream(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint8_t stopByte;

    shell_printf("streaming - press any key to stop\r\n");

    for (;;) {
        imu_sample_t accel;
        imu_sample_t gyro;

        if (imu_get_latest_accel(&accel) && imu_get_latest_gyro(&gyro)) {
            shell_printf("accel x=%6d y=%6d z=%6d  gyro x=%6d y=%6d z=%6d\r\n",
                         accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z);
        }

        if (xStreamBufferReceive(shellRxStreamBuf, &stopByte, 1, pdMS_TO_TICKS(100)) > 0) {
            break;
        }
    }

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

static const char *logger_state_name(logger_state_t state)
{
    switch (state) {
    case LOGGER_IDLE:   return "idle";
    case LOGGER_ACTIVE: return "active";
    case LOGGER_ERROR:  return "error";
    default:             return "unknown";
    }
}

#define CMD_LOG_USAGE "usage: log start|stop|status|list|dump <file>\r\n"

/* logger_request_*() only post to loggerTask's control queue and
   return immediately — the actual f_mount()/f_open()/f_read()/
   f_write()/f_close() happens over in loggerTask, never here. That's
   why this can't report success/failure synchronously; run `log
   status` (or just watch the output) a moment later to see how it
   landed. `list`/`dump` are a testing-only readback path over this same
   UART — see logger.h for why, and don't run `dump` while a session is
   active. */
static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(CMD_LOG_USAGE);
        return 0;
    }

    if (strcmp(argv[1], "start") == 0) {
        logger_request_start();
        shell_printf("log: start requested\r\n");
    } else if (strcmp(argv[1], "stop") == 0) {
        logger_request_stop();
        shell_printf("log: stop requested\r\n");
    } else if (strcmp(argv[1], "status") == 0) {
        shell_printf("log: %s, %lu records written\r\n",
                     logger_state_name(logger_get_state()),
                     (unsigned long)logger_get_record_count());
    } else if (strcmp(argv[1], "list") == 0) {
        logger_request_list();
    } else if (strcmp(argv[1], "dump") == 0) {
        if (argc < 3) {
            shell_printf(CMD_LOG_USAGE);
        } else {
            logger_request_dump(argv[2]);
        }
    } else {
        shell_printf(CMD_LOG_USAGE);
    }

    return 0;
}

#define CMD_CAN_USAGE "usage: can status[1|2]|stby<1|2> on|off|term<1|2> on|off|send<1|2> <id> <hex> [fd] [brs]|stream<1|2>\r\n"

/* Matches "stby1"/"stby2", "term1"/"term2", etc. against a prefix and
   sets *bus from the trailing digit. Returns false (bus untouched) if
   tok doesn't match prefix+1 or prefix+2 exactly. */
static bool parse_bus_suffix(const char *tok, const char *prefix, can_bus_t *bus)
{
    size_t prefixLen = strlen(prefix);

    if (strncmp(tok, prefix, prefixLen) != 0) {
        return false;
    }
    if (strcmp(tok + prefixLen, "1") == 0) {
        *bus = CAN_BUS_1;
        return true;
    }
    if (strcmp(tok + prefixLen, "2") == 0) {
        *bus = CAN_BUS_2;
        return true;
    }
    return false;
}

static bool hex_nibble(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

/* Decodes a contiguous hex string ("DEADBEEF") into out[]. Returns the
   byte count, or -1 on an odd-length or non-hex string, or one that
   wouldn't fit in out_max bytes. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_max)
{
    size_t hexLen = strlen(hex);
    if ((hexLen % 2u) != 0u) {
        return -1;
    }

    size_t byteCount = hexLen / 2u;
    if (byteCount > out_max) {
        return -1;
    }

    for (size_t i = 0; i < byteCount; i++) {
        uint8_t hi, lo;
        if (!hex_nibble(hex[i * 2u], &hi) || !hex_nibble(hex[i * 2u + 1u], &lo)) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return (int)byteCount;
}

static void can_print_status(can_bus_t bus, const char *name)
{
    can_status_t st;

    if (!can_get_status(bus, &st)) {
        shell_printf("%s  status read failed\r\n", name);
        return;
    }

    shell_printf("%s  standby=%u term=%u fd_capable=%u bus_off=%u err_passive=%u\r\n",
                 name, (unsigned)st.standby, (unsigned)st.terminated, (unsigned)st.fd_capable,
                 (unsigned)st.bus_off, (unsigned)st.error_passive);
    shell_printf("     last_err=0x%02lX tx_err=%lu rx_err=%lu\r\n",
                 (unsigned long)st.last_error_code, (unsigned long)st.tx_error_count,
                 (unsigned long)st.rx_error_count);
    shell_printf("     rx=%lu tx=%lu rx_drop=%lu tx_drop=%lu\r\n",
                 (unsigned long)st.rx_count, (unsigned long)st.tx_count,
                 (unsigned long)st.rx_drop_count, (unsigned long)st.tx_drop_count);
}

/* argv layout: {"can", "send<1|2>", "<id hex>", "<data hex>", ["fd"], ["brs"]}.
   id is parsed as hex (CAN-convention, no "0x" prefix, e.g. "123" means
   0x123) and is auto-classified standard/extended by whether it fits
   in 11 bits -- there's no explicit override for forcing extended
   addressing on a value that would also fit as standard, which this
   bring-up tool doesn't need yet. */
static int can_do_send(can_bus_t bus, int argc, char **argv)
{
    if (argc < 4) {
        shell_printf(CMD_CAN_USAGE);
        return 0;
    }

    char *end;
    unsigned long id = strtoul(argv[2], &end, 16);
    if (*end != '\0') {
        shell_printf("can: bad id '%s'\r\n", argv[2]);
        return 0;
    }

    uint8_t data[64];
    int len = hex_decode(argv[3], data, sizeof(data));
    if (len < 0) {
        shell_printf("can: bad hex payload '%s'\r\n", argv[3]);
        return 0;
    }

    bool is_fd = false;
    bool is_brs = false;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "fd") == 0) {
            is_fd = true;
        } else if (strcmp(argv[i], "brs") == 0) {
            is_brs = true;
        }
    }

    bool is_extended = (id > 0x7FFu);

    if (can_send(bus, (uint32_t)id, data, (uint8_t)len, is_extended, is_fd, is_brs)) {
        shell_printf("can: queued %d bytes on id 0x%lX\r\n", len, id);
    } else {
        shell_printf("can: send failed (queue full, fd requested on a classic-only bus,"
                     " or >8 bytes without 'fd')\r\n");
    }

    return 0;
}

/* Borrows cmd_imustream()'s idiom for the stop-key check
   (xStreamBufferReceive()'s timeout), but prints only on a genuinely
   new frame (can_get_new_rx()'s sequence check) rather than
   re-printing the same cached one every poll -- unlike IMU's
   continuous sampling, CAN traffic is event-driven, so a fixed-rate
   "heartbeat" print was just noise. Still a cached single-frame
   snapshot, not the raw per-frame queue canRxTask drains (a second
   consumer of that queue would race it) -- a burst of several new
   frames within one 100 ms poll interval still only shows the latest
   of them. */
static int can_stream(can_bus_t bus)
{
    uint8_t stopByte;
    uint32_t lastSeq = 0;

    shell_printf("streaming - press any key to stop\r\n");

    for (;;) {
        can_frame_t frame;
        if (can_get_new_rx(bus, &frame, &lastSeq)) {
            shell_printf("id=0x%03lX %s%s len=%u data=",
                         (unsigned long)frame.id,
                         frame.is_extended ? "ext " : "std ",
                         frame.is_fd ? (frame.is_brs ? "fd+brs " : "fd ") : "",
                         frame.len);
            for (uint8_t i = 0; i < frame.len; i++) {
                shell_printf("%02X", frame.data[i]);
            }
            shell_printf("\r\n");
        }

        if (xStreamBufferReceive(shellRxStreamBuf, &stopByte, 1, pdMS_TO_TICKS(100)) > 0) {
            break;
        }
    }

    return 0;
}

static int cmd_can(int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(CMD_CAN_USAGE);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        can_print_status(CAN_BUS_1, "CAN1");
        can_print_status(CAN_BUS_2, "CAN2");
        return 0;
    }

    can_bus_t bus;

    if (parse_bus_suffix(argv[1], "status", &bus)) {
        can_print_status(bus, (bus == CAN_BUS_1) ? "CAN1" : "CAN2");
        return 0;
    }


    if (parse_bus_suffix(argv[1], "stby", &bus)) {
        if (argc < 3) {
            shell_printf(CMD_CAN_USAGE);
        } else if (strcmp(argv[2], "on") == 0) {
            can_set_standby(bus, true);
            shell_printf("can: standby on\r\n");
        } else if (strcmp(argv[2], "off") == 0) {
            can_set_standby(bus, false);
            shell_printf("can: standby off\r\n");
        } else {
            shell_printf(CMD_CAN_USAGE);
        }
        return 0;
    }

    if (parse_bus_suffix(argv[1], "term", &bus)) {
        if (argc < 3) {
            shell_printf(CMD_CAN_USAGE);
        } else if (strcmp(argv[2], "on") == 0) {
            can_set_termination(bus, true);
            shell_printf("can: termination on\r\n");
        } else if (strcmp(argv[2], "off") == 0) {
            can_set_termination(bus, false);
            shell_printf("can: termination off\r\n");
        } else {
            shell_printf(CMD_CAN_USAGE);
        }
        return 0;
    }

    if (parse_bus_suffix(argv[1], "send", &bus)) {
        return can_do_send(bus, argc, argv);
    }

    if (parse_bus_suffix(argv[1], "stream", &bus)) {
        return can_stream(bus);
    }

    shell_printf(CMD_CAN_USAGE);
    return 0;
}

/* mg/mdps-style integer-only display, matching cmd_imu's rationale
   exactly: --specs=nano.specs without -u _printf_float means %f
   mis-renders, so the volts figure is built from plain integer
   division/modulo instead of trusting vsnprintf with a float. */
static int cmd_vbat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t raw;
    uint32_t mv;
    if (!vbat_get_reading(&raw, &mv)) {
        shell_printf("vbat: no sample yet\r\n");
        return 0;
    }

    shell_printf("vbat  raw=%lu  mv=%lu  %lu.%02lu V\r\n",
                 (unsigned long)raw, (unsigned long)mv,
                 (unsigned long)(mv / 1000u), (unsigned long)((mv % 1000u) / 10u));

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
