#ifndef APP_INC_LOGGER_H
#define APP_INC_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

/* Creates logQueue and the small control queue `log start`/`log stop`
   use to talk to loggerTask. Must run before loggerTask (or anything
   that calls logger_post()) is created — see tasks_init(). False if
   either queue could not be created. */
bool logger_init(void);

/* All FatFS calls (f_mount/f_open/f_write/f_sync/f_close) happen only
   here, in loggerTask's own context — never from shellTask. Blocks on
   logQueue with a 1 s timeout: a queued record gets written if logging
   is active, and the timeout doubles as a periodic f_sync() so a power
   cut doesn't leave the file's FAT metadata stale (and the file reading
   as 0 bytes), the failure that actually matters for a datalogger. */
void loggerTask(void *argument);

/* Posts one timestamped sample onto logQueue for loggerTask to write,
   tagging it 'A' (accel) or 'G' (gyro) in the CSV. Non-blocking (0
   timeout) and silently drops the sample if the queue is full — a slow
   SD write must never stall imuTask's own interrupt-driven pipeline,
   which is the higher-priority concern. A cheap no-op when logging
   isn't active, so producers can call this unconditionally. */
void logger_post(char sensor, int16_t x, int16_t y, int16_t z);

/* `log start` / `log stop` / `log status` (shell.c) call these. Both
   post to loggerTask's control queue and return immediately — the
   actual mount/open/close happens asynchronously in loggerTask, never
   on shellTask's stack. */
void logger_request_start(void);
void logger_request_stop(void);

/* Testing-only readback path, over the same UART the shell already
   uses (tio's own --log-file capture is the intended way to pull it
   into a file on the PC) rather than a physical card reader. Same
   async pattern as start/stop: posts a request, returns immediately,
   and loggerTask does the actual f_opendir()/f_open()/f_read() and
   shell_printf()s the result. Don't run `log dump` while a session is
   LOGGER_ACTIVE -- it blocks loggerTask for the whole transfer (each
   line waits on printQueue, throttled to UART speed), during which
   incoming samples just queue up in logQueue and can overflow. */
void logger_request_list(void);
void logger_request_dump(const char *filename);

typedef enum {
    LOGGER_IDLE,     /* not logging, no file open */
    LOGGER_ACTIVE,   /* file open, writing records as they arrive */
    LOGGER_ERROR,    /* last start attempt failed (no card, mount, or open error) */
} logger_state_t;

/* Current state plus, when LOGGER_ACTIVE, how many records have been
   written to the open file so far. Safe to call from any task: both
   are single, naturally-aligned words written only by loggerTask, which
   is atomic on this Cortex-M4 without needing a lock. */
logger_state_t logger_get_state(void);
uint32_t logger_get_record_count(void);

#endif /* APP_INC_LOGGER_H */