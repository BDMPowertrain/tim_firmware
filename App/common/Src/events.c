/* events.c — the image's Event log, see events.h.

   The log's state (event_log_t, with its 2 KB repair buffer) is static and
   shared by the writer and the shell's read-out under logMutex. The shell
   holds it for one read at a time and never while printing, as a print
   can wait on the UART. Each flash operation also holds flash_bank2's own
   mutex, which the Parameter saves share. */

#include "events.h"

#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include "build_id.h"
#include "event_codes.h"
#include "event_flood.h"
#include "flash_bank2.h"
#include "print.h"
#include "reset_info.h"

/* One queued Event: the writer adds the boot and sequence numbers. */
typedef struct {
    uint32_t uptime_ms;
    uint16_t code;
    uint8_t  data[EVENT_DATA_SIZE];
} queued_event_t;

/* Newest Events the read-out prints by default, and at most. */
#define EVENTS_SHOW_DEFAULT 16u
#define EVENTS_SHOW_MAX     (EVENT_LOG_PAGES * EVENT_LOG_SLOTS)

static osMessageQueueId_t eventQueue;
static osMutexId_t logMutex;

/* Events that found the queue full. Every posting task adds to it with an
   atomic add (LDREX/STREX), so the Control step takes no lock. */
static uint32_t queueDropped;

/* Under logMutex once the writer runs. */
static event_log_t log_;
static bool opened;

/* Written by the writer only; the shell reads it whole (one 32-bit load). */
static uint32_t flooded; /* refused by the flood guard */

bool events_init(void)
{
    static const osMutexAttr_t attr = {
        .name = "eventLogMutex",
        .attr_bits = osMutexPrioInherit,
    };

    eventQueue = osMessageQueueNew(EVENTS_QUEUE_DEPTH, sizeof(queued_event_t), NULL);
    logMutex = osMutexNew(&attr);
    return eventQueue != NULL && logMutex != NULL;
}

bool events_post(uint16_t code, const uint8_t data[EVENT_DATA_SIZE])
{
    queued_event_t item = {
        .uptime_ms = osKernelGetTickCount(),
        .code = code,
    };

    memcpy(item.data, data, EVENT_DATA_SIZE);
    if (eventQueue == NULL || osMessageQueuePut(eventQueue, &item, 0u, 0u) != osOK) {
        (void)__atomic_fetch_add(&queueDropped, 1u, __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

uint32_t events_queued(void)
{
    return eventQueue != NULL ? osMessageQueueGetCount(eventQueue) : 0u;
}

static void lock(void)
{
    (void)osMutexAcquire(logMutex, osWaitForever);
}

static void unlock(void)
{
    (void)osMutexRelease(logMutex);
}

/* ---- the writer -------------------------------------------------------- */

static void store(uint16_t code, uint32_t uptime_ms, const uint8_t data[EVENT_DATA_SIZE])
{
    lock();
    (void)event_log_append(&log_, code, uptime_ms, data); /* failures are counted in log_ */
    unlock();
}

/* The boot Event, then the records the last reset left (event_codes.h). */
static void write_boot(const events_task_args_t *args, uint32_t now_ms)
{
    const reset_report_t *reset = reset_info_report();
    const event_boot_t boot = {
        .image = args->image,
        .build_id = BUILD_ID,
        .dirty = BUILD_DIRTY != 0,
        .reset = reset,
    };
    uint8_t data[EVENT_DATA_SIZE];

    event_data_boot(data, &boot);
    store(EVENT_BOOT, now_ms, data);

    /* Kept before data is reused for what follows. */
    const uint8_t flags = data[2];

    if ((flags & EVENT_BOOT_CRASH_NEXT) != 0u) {
        event_data_crash(data, &reset->crash);
        store(EVENT_BOOT_CRASH, now_ms, data);
    }
    if ((flags & EVENT_BOOT_CRUMB_NEXT) != 0u) {
        event_data_breadcrumb(data, &reset->breadcrumb);
        store(EVENT_BOOT_BREADCRUMB, now_ms, data);
    }
}

void eventTask(void *argument)
{
    const events_task_args_t *args = (const events_task_args_t *)argument;
    const flash_if_t *flash = flash_bank2_get();
    uint8_t data[EVENT_DATA_SIZE];
    event_flood_t flood;
    uint32_t dropSeen = 0u;
    uint32_t now = osKernelGetTickCount();

    lock();
    event_log_open(&log_, flash); /* repairs here: erases stall only bank 2 (ADR 0014) */
    opened = true;
    unlock();

    write_boot(args, now);
    for (uint32_t i = 0; i < log_.repair_count; i++) {
        event_data_repair(data, &log_.repair[i], log_.repairs);
        store(EVENT_STORAGE_REPAIRED, now, data);
    }
    if (flash == NULL) {
        memset(data, 0, sizeof data);
        store(EVENT_NOT_DUAL_BANK, now, data);
    }

    event_flood_init(&flood, now);
    for (;;) {
        queued_event_t item;
        /* Wakes at least once per refill, so a report of drops is not held
           back until the next Event arrives. */
        const bool got = osMessageQueueGet(eventQueue, &item, NULL, EVENT_FLOOD_REFILL_MS) == osOK;
        const uint32_t dropped = __atomic_load_n(&queueDropped, __ATOMIC_RELAXED);

        now = osKernelGetTickCount();
        event_flood_add_dropped(&flood, dropped - dropSeen);
        dropSeen = dropped;

        const uint32_t report = event_flood_take_report(&flood, now);

        if (report > 0u) {
            event_data_dropped(data, report);
            store(EVENT_EVENTS_DROPPED, now, data);
        }
        if (got) {
            if (event_flood_admit(&flood, now)) {
                store(item.code, item.uptime_ms, item.data);
            } else {
                flooded++;
            }
        }
    }
}

/* ---- the read-out ------------------------------------------------------ */

static void print_event(const event_t *ev)
{
    /* The data as hex in one line, so the line is one print record. */
    static const char digits[] = "0123456789ABCDEF";
    char hex[EVENT_DATA_SIZE * 3u + 2u];
    char *at = hex;

    for (uint32_t i = 0; i < EVENT_DATA_SIZE; i++) {
        if (i == 8u) {
            *at++ = ' ';
        }
        *at++ = ' ';
        *at++ = digits[ev->data[i] >> 4];
        *at++ = digits[ev->data[i] & 0x0Fu];
    }
    *at = '\0';
    shell_printf("%5u #%-4lu %9lu ms  %-22s%s\r\n", (unsigned)ev->boot, (unsigned long)ev->seq,
                 (unsigned long)ev->uptime_ms, event_code_name(ev->code), hex);
}

/* One read under the lock: the writer may move the head between two. */
static event_read_t read_at(event_pos_t pos, event_t *ev)
{
    lock();
    const event_read_t r = event_log_read(&log_, pos, ev);
    unlock();
    return r;
}

static void print_state(void)
{
    lock();
    const bool noFlash = log_.flash == NULL;
    const uint16_t boot = log_.boot;
    const uint32_t seq = log_.seq;
    const uint32_t repairs = log_.repairs;
    const uint32_t failures = log_.write_failures;
    unlock();

    if (noFlash) {
        shell_printf("events: not dual-bank, nothing is stored\r\n");
    }
    shell_printf("events: boot %u, %lu written this start, storage repaired %lu, "
                 "write failures %lu\r\n",
                 (unsigned)boot, (unsigned long)seq, (unsigned long)repairs,
                 (unsigned long)failures);
    shell_printf("events: queued %lu, dropped queue full %lu, flood %lu\r\n",
                 (unsigned long)events_queued(),
                 (unsigned long)__atomic_load_n(&queueDropped, __ATOMIC_RELAXED),
                 (unsigned long)flooded);
}

int events_cmd(int argc, char **argv)
{
    uint32_t want = EVENTS_SHOW_DEFAULT;

    if (argc == 2) {
        char *end;
        const unsigned long n = strtoul(argv[1], &end, 10);

        if (*argv[1] == '\0' || *end != '\0' || n == 0u) {
            shell_printf("usage: events [n]\r\n");
            return 0;
        }
        want = n < EVENTS_SHOW_MAX ? (uint32_t)n : EVENTS_SHOW_MAX;
    } else if (argc > 2) {
        shell_printf("usage: events [n]\r\n");
        return 0;
    }

    if (!opened) {
        shell_printf("events: the writer has not opened the log yet\r\n");
        return 0;
    }
    print_state();

    /* Back from the newest by want slots, then forward to it. */
    event_pos_t pos;
    uint32_t shown = 1u;

    lock();
    bool any = event_log_newest(&log_, &pos);
    while (any && shown < want && event_log_older(&log_, &pos)) {
        shown++;
    }
    unlock();
    if (!any) {
        shell_printf("events: none\r\n");
        return 0;
    }

    shell_printf(" boot seq        uptime  event                  data\r\n");
    for (uint32_t i = 0; i < shown; i++) {
        event_t ev;

        switch (read_at(pos, &ev)) {
        case EVENT_READ_OK:
            print_event(&ev);
            break;
        case EVENT_READ_BLANK:
            /* The writer has erased this page since the walk began. */
            shell_printf("  (log moved on while printing)\r\n");
            return 0;
        case EVENT_READ_BAD_CRC:
            shell_printf("  (slot %u.%u skipped: bad CRC)\r\n", (unsigned)pos.page,
                         (unsigned)pos.slot);
            break;
        default:
            shell_printf("  (slot %u.%u skipped: read fault)\r\n", (unsigned)pos.page,
                         (unsigned)pos.slot);
            break;
        }
        lock();
        any = event_log_newer(&log_, &pos);
        unlock();
        if (!any) {
            break;
        }
    }
    return 0;
}
