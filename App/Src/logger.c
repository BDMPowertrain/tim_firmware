/* logger.c — SD-card CSV logging. Every FatFS call (f_mount, f_open,
   f_write, f_sync, f_close) happens here, in loggerTask's own context,
   never from shellTask: the ff.c call chain plus user_diskio.c's SD-SPI
   driver is deep, and shellTask already proved it can run its stack
   thin (Documentation/tim-rtos-architecture.md, the stack-overflow
   bring-up). Two queues: logQueue carries data records from producers
   (imu.c), a separate small control queue carries `log start`/`log
   stop` requests from the shell — kept apart so a control request never
   has to fight through however many data records are already queued. */

#include "logger.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

#include "app_fatfs.h"
#include "main.h"
#include "print.h"

typedef struct {
    uint32_t timestamp_ms;
    char     sensor;   /* 'A' accel, 'G' gyro */
    int16_t  x, y, z;
} log_record_t;

typedef enum {
    LOG_CMD_START,
    LOG_CMD_STOP,
    LOG_CMD_LIST,
    LOG_CMD_DUMP,
} log_cmd_t;

/* filename is only meaningful for LOG_CMD_DUMP; 13 = 8.3 name + '.' +
   nul, matching _USE_LFN==0 (ffconf.h). */
typedef struct {
    log_cmd_t cmd;
    char filename[13];
} log_ctrl_msg_t;

#define LOG_QUEUE_DEPTH     64u
#define LOG_CTRL_QUEUE_DEPTH 4u
#define LOG_SYNC_PERIOD_MS  1000u
#define LOG_DUMP_CHUNK      64u

static osMessageQueueId_t logQueue;
static osMessageQueueId_t logCtrlQueue;

static volatile logger_state_t loggerState = LOGGER_IDLE;
static volatile uint32_t loggerRecordCount;

void logger_init(void)
{
    logQueue = osMessageQueueNew(LOG_QUEUE_DEPTH, sizeof(log_record_t), NULL);
    logCtrlQueue = osMessageQueueNew(LOG_CTRL_QUEUE_DEPTH, sizeof(log_ctrl_msg_t), NULL);
    if (logQueue == NULL || logCtrlQueue == NULL) {
        Error_Handler();
    }
}

void logger_post(char sensor, int16_t x, int16_t y, int16_t z)
{
    if (loggerState != LOGGER_ACTIVE) {
        return;
    }

    log_record_t record = {
        .timestamp_ms = HAL_GetTick(),
        .sensor = sensor,
        .x = x,
        .y = y,
        .z = z,
    };

    /* 0 timeout: never block the caller (imuTask, from its own
       interrupt-driven FIFO drain) waiting for queue space. */
    osMessageQueuePut(logQueue, &record, 0, 0);
}

void logger_request_start(void)
{
    log_ctrl_msg_t msg = { .cmd = LOG_CMD_START };
    osMessageQueuePut(logCtrlQueue, &msg, 0, 0);
}

void logger_request_stop(void)
{
    log_ctrl_msg_t msg = { .cmd = LOG_CMD_STOP };
    osMessageQueuePut(logCtrlQueue, &msg, 0, 0);
}

void logger_request_list(void)
{
    log_ctrl_msg_t msg = { .cmd = LOG_CMD_LIST };
    osMessageQueuePut(logCtrlQueue, &msg, 0, 0);
}

void logger_request_dump(const char *filename)
{
    log_ctrl_msg_t msg = { .cmd = LOG_CMD_DUMP };
    strncpy(msg.filename, filename, sizeof(msg.filename) - 1u);
    msg.filename[sizeof(msg.filename) - 1u] = '\0';
    osMessageQueuePut(logCtrlQueue, &msg, 0, 0);
}

logger_state_t logger_get_state(void)
{
    return loggerState;
}

uint32_t logger_get_record_count(void)
{
    return loggerRecordCount;
}

/* Scans for the first LOG%03u.CSV that doesn't exist yet, 1..999, so
   every `log start` gets its own file instead of overwriting the last
   session. No RTC/wall-clock time source on this board, so a sequential
   number is the simplest naming scheme that doesn't depend on one. */
static bool logger_next_filename(char *out, size_t out_len)
{
    FILINFO fno;

    for (uint32_t n = 1; n <= 999u; n++) {
        snprintf(out, out_len, "LOG%03lu.CSV", (unsigned long)n);
        if (f_stat(out, &fno) == FR_NO_FILE) {
            return true;
        }
    }

    return false;
}

/* Attempts to mount the card (if not already) and open a new log file.
   Leaves loggerState in LOGGER_ACTIVE on success, LOGGER_ERROR
   otherwise — never leaves a half-open file behind. */
static void logger_handle_start(void)
{
    if (loggerState == LOGGER_ACTIVE) {
        return;
    }

    if (f_mount(&USERFatFs, USERPath, 1) != FR_OK) {
        shell_printf("log: mount failed (no card, or not FAT32?)\r\n");
        loggerState = LOGGER_ERROR;
        return;
    }

    char filename[16];
    if (!logger_next_filename(filename, sizeof(filename))) {
        shell_printf("log: no free LOG###.CSV name (999 files already present)\r\n");
        loggerState = LOGGER_ERROR;
        return;
    }

    if (f_open(&USERFile, filename, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        shell_printf("log: could not open %s\r\n", filename);
        loggerState = LOGGER_ERROR;
        return;
    }

    static const char header[] =
        "# accel: raw LSB, FS=+/-8g, 0.244 mg/LSB | gyro: raw LSB, FS=+/-2000dps, 70 mdps/LSB\r\n"
        "ms,sensor,x,y,z\r\n";
    UINT written;
    f_write(&USERFile, header, sizeof(header) - 1u, &written);

    loggerRecordCount = 0;
    loggerState = LOGGER_ACTIVE;
    shell_printf("log: started %s\r\n", filename);
}

static void logger_handle_stop(void)
{
    if (loggerState != LOGGER_ACTIVE) {
        return;
    }

    f_close(&USERFile);
    loggerState = LOGGER_IDLE;
    shell_printf("log: stopped (%lu records)\r\n", (unsigned long)loggerRecordCount);
}

/* Mounts (if needed) and lists every file in the root directory with
   its size, via shell_printf(). Doesn't care whether a session is
   active -- reads only, never touches USERFile. */
static void logger_handle_list(void)
{
    if (f_mount(&USERFatFs, USERPath, 1) != FR_OK) {
        shell_printf("log: mount failed (no card, or not FAT32?)\r\n");
        return;
    }

    DIR dir;
    if (f_opendir(&dir, "") != FR_OK) {
        shell_printf("log: could not open root directory\r\n");
        return;
    }

    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        shell_printf("%-13s %lu bytes\r\n", fno.fname, (unsigned long)fno.fsize);
    }

    f_closedir(&dir);
}

/* Streams one file's contents out over the shell UART, chunked through
   shell_print_raw() -- which blocks on printQueue when it's full, so
   this is naturally throttled to UART speed rather than overrunning it.
   That's also why it isn't safe to run during an active logging
   session: it can hold loggerTask here for the whole transfer. */
static void logger_handle_dump(const char *filename)
{
    if (f_mount(&USERFatFs, USERPath, 1) != FR_OK) {
        shell_printf("log: mount failed (no card, or not FAT32?)\r\n");
        return;
    }

    FIL f;
    if (f_open(&f, filename, FA_READ) != FR_OK) {
        shell_printf("log: could not open %s\r\n", filename);
        return;
    }

    char buf[LOG_DUMP_CHUNK];
    UINT rd;
    do {
        if (f_read(&f, buf, sizeof(buf), &rd) != FR_OK) {
            break;
        }
        if (rd > 0u) {
            shell_print_raw(buf, rd);
        }
    } while (rd == sizeof(buf));

    f_close(&f);
}

static void logger_write_record(const log_record_t *record)
{
    char line[48];
    int len = snprintf(line, sizeof(line), "%lu,%c,%d,%d,%d\r\n",
                        (unsigned long)record->timestamp_ms, record->sensor,
                        record->x, record->y, record->z);
    if (len <= 0) {
        return;
    }

    UINT written;
    if (f_write(&USERFile, line, (UINT)len, &written) != FR_OK || written != (UINT)len) {
        /* Card pulled mid-log, or a write error: stop rather than keep
           calling into a filesystem that's no longer trustworthy. */
        shell_printf("log: write error, stopping\r\n");
        f_close(&USERFile);
        loggerState = LOGGER_ERROR;
        return;
    }

    loggerRecordCount++;
}

void loggerTask(void *argument)
{
    (void)argument;

    uint32_t lastSync = HAL_GetTick();

    for (;;) {
        log_ctrl_msg_t msg;
        if (osMessageQueueGet(logCtrlQueue, &msg, NULL, 0) == osOK) {
            switch (msg.cmd) {
            case LOG_CMD_START:
                logger_handle_start();
                break;
            case LOG_CMD_STOP:
                logger_handle_stop();
                break;
            case LOG_CMD_LIST:
                logger_handle_list();
                break;
            case LOG_CMD_DUMP:
                logger_handle_dump(msg.filename);
                break;
            }
        }

        log_record_t record;
        if (osMessageQueueGet(logQueue, &record, NULL, LOG_SYNC_PERIOD_MS) == osOK) {
            if (loggerState == LOGGER_ACTIVE) {
                logger_write_record(&record);
            }
        }

        if (loggerState == LOGGER_ACTIVE && (HAL_GetTick() - lastSync) >= LOG_SYNC_PERIOD_MS) {
            f_sync(&USERFile);
            lastSync = HAL_GetTick();
        }
    }
}