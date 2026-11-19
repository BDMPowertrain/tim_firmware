/* data_log.c — the Data log's SD logger, see data_log.h.

   Every FatFS call happens on this task. The card, the batch and the file
   are this task's alone; the shell reads a copy of the status under
   statusMutex. FatFS's own objects are CubeMX's (app_fatfs.h): one volume,
   one file open at a time. */

#include "data_log.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

#include "app_fatfs.h"
#include "build_id.h"
#include "control_step.h"
#include "data_log_card.h"
#include "data_log_record.h"
#include "data_log_ring.h"
#include "event_codes.h"
#include "events.h"
#include "log_batch.h"
#include "print.h"

/* How often the logger empties the rings: well inside the 640 ms the VCU's
   64-deep step ring holds. */
#define DATA_LOG_POLL_MS 20u

/* Files deleted at most in one go to make room, so a card full of other
   files cannot keep the logger here. */
#define DATA_LOG_DELETES_MAX 64u

/* "B00042/L0003.TDL" and its NUL. */
#define DATA_LOG_PATH_MAX (2u * DATA_LOG_NAME_MAX)

typedef struct {
    data_log_state_t state;
    char     file[DATA_LOG_PATH_MAX]; /* the file open, "" if none */
    uint32_t written;     /* Records put in a file */
    uint32_t not_written; /* Records taken from a ring with no file open */
    uint32_t files;       /* files opened since power-on */
    uint32_t deleted;     /* files deleted to make room */
    uint8_t  last_result; /* the last FatFS failure (FRESULT) */
} data_log_status_t;

static const data_log_def_t *def;

static osMutexId_t statusMutex;
static data_log_status_t shared; /* under statusMutex */

/* The task's own. */
static data_log_status_t status;
static data_log_card_t card;
static log_batch_t batch;
static bool mounted;
static bool fileOpen;
static uint16_t boot;
static uint8_t payload[DATA_LOG_PAYLOAD_MAX];
static uint8_t encoded[DATA_LOG_PAYLOAD_MAX + DATA_LOG_RECORD_OVERHEAD];

bool data_log_init(const data_log_def_t *d)
{
    static const osMutexAttr_t attr = {
        .name = "dataLogStatus",
        .attr_bits = osMutexPrioInherit,
    };

    def = d;
    statusMutex = osMutexNew(&attr);
    for (size_t i = 0; i < DATA_LOG_RING_COUNT; i++) {
        if (d->rings[i].payload_max > DATA_LOG_PAYLOAD_MAX) {
            return false;
        }
    }
    if (d->preamble_len > DATA_LOG_PAYLOAD_MAX) {
        return false;
    }
    return data_log_rings_open(d->rings) && statusMutex != NULL;
}

static void publish_status(void)
{
    status.state = card.state;
    data_log_set_state(card.state);
    (void)osMutexAcquire(statusMutex, osWaitForever);
    shared = status;
    (void)osMutexRelease(statusMutex);
}

/* ---- the file ---------------------------------------------------------- */

static FRESULT make_space(void);

/* Writes the batch's whole sectors (all of it if all). A short write is a
   full card: the oldest files are deleted and the rest written once more;
   still short is a failure. */
static FRESULT write_batch(bool all)
{
    for (uint32_t attempt = 0; attempt < 2u; attempt++) {
        const size_t n = all ? log_batch_used(&batch) : log_batch_sectors(&batch);
        UINT done = 0u;

        if (n == 0u) {
            return FR_OK;
        }
        FRESULT fr = f_write(&USERFile, log_batch_data(&batch), (UINT)n, &done);
        /* What went out is in the file, whatever happened to the rest. */
        log_batch_consume(&batch, done);
        if (fr != FR_OK) {
            return fr;
        }
        if (done == n) {
            return FR_OK;
        }
        if (attempt == 0u) {
            fr = make_space();
            if (fr != FR_OK) {
                return fr;
            }
        }
    }
    return FR_DENIED;
}

static FRESULT put(const uint8_t *data, size_t len)
{
    while (len > 0u) {
        const size_t taken = log_batch_put(&batch, data, len);

        data += taken;
        len -= taken;
        if (log_batch_full(&batch)) {
            const FRESULT fr = write_batch(false);
            if (fr != FR_OK) {
                return fr;
            }
        }
    }
    return FR_OK;
}

/* The next file number free in dir: one more than the highest there. */
static FRESULT next_file_number(const char *dir, uint16_t *number)
{
    DIR d;
    FILINFO fno;
    uint16_t highest = 0u;
    FRESULT fr = f_opendir(&d, dir);

    if (fr != FR_OK) {
        return fr;
    }
    while ((fr = f_readdir(&d, &fno)) == FR_OK && fno.fname[0] != '\0') {
        uint16_t n;
        if (data_log_parse_file(fno.fname, &n) && n > highest) {
            highest = n;
        }
    }
    (void)f_closedir(&d);
    if (fr != FR_OK) {
        return fr;
    }
    if (highest >= 9999u) {
        return FR_DENIED;
    }
    *number = (uint16_t)(highest + 1u);
    return FR_OK;
}

/* A new file in this boot's directory, its header in the batch. */
static FRESULT open_file(uint32_t now)
{
    char dir[DATA_LOG_NAME_MAX];
    char name[DATA_LOG_NAME_MAX];
    char path[DATA_LOG_PATH_MAX];
    uint16_t number = 0u;

    data_log_dir_name(boot, dir);
    FRESULT fr = f_mkdir(dir);
    if (fr != FR_OK && fr != FR_EXIST) {
        return fr;
    }
    fr = next_file_number(dir, &number);
    if (fr != FR_OK) {
        return fr;
    }
    data_log_file_name(number, name);
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    fr = f_open(&USERFile, path, FA_WRITE | FA_CREATE_NEW);
    if (fr != FR_OK) {
        return fr;
    }
    fileOpen = true;

    const data_log_header_t header = {
        .image = def->image,
        .dirty = BUILD_DIRTY != 0,
        .build_id = BUILD_ID,
        .boot = boot,
        .file = number,
        .opened_ms = now,
    };
    uint8_t bytes[DATA_LOG_HEADER_SIZE];

    data_log_header_encode(bytes, &header);
    log_batch_init(&batch);
    fr = put(bytes, sizeof bytes);
    if (fr != FR_OK) {
        return fr;
    }
    if (def->preamble_len > 0u) {
        const record_head_t head = {
            .uptime_ms = now,
            .seq = 0u,
            .len = def->preamble_len,
            .type = def->preamble_type,
        };
        fr = put(encoded, data_log_record_encode(encoded, sizeof encoded, &head,
                                                 (const uint8_t *)def->preamble));
        if (fr != FR_OK) {
            return fr;
        }
    }
    data_log_card_opened(&card, now);
    (void)memcpy(status.file, path, sizeof path);
    status.files++;
    return FR_OK;
}

static FRESULT close_file(void)
{
    FRESULT fr = write_batch(true);
    const FRESULT closed = f_close(&USERFile);

    fileOpen = false;
    status.file[0] = '\0';
    return fr != FR_OK ? fr : closed;
}

/* ---- room on the card --------------------------------------------------- */

/* The oldest boot's directory: the one furthest behind this boot. */
static FRESULT oldest_dir(char dir[DATA_LOG_NAME_MAX])
{
    DIR d;
    FILINFO fno;
    bool found = false;
    uint16_t oldest = 0u;
    FRESULT fr = f_opendir(&d, "");

    if (fr != FR_OK) {
        return fr;
    }
    while ((fr = f_readdir(&d, &fno)) == FR_OK && fno.fname[0] != '\0') {
        uint16_t b;
        if ((fno.fattrib & AM_DIR) != 0u && data_log_parse_dir(fno.fname, &b) &&
            (!found || data_log_boot_older(b, oldest, boot))) {
            oldest = b;
            found = true;
        }
    }
    (void)f_closedir(&d);
    if (fr != FR_OK) {
        return fr;
    }
    if (!found) {
        return FR_NO_FILE;
    }
    data_log_dir_name(oldest, dir);
    return FR_OK;
}

/* Deletes the oldest log file, or the oldest boot's directory once it is
   empty. FR_NO_FILE: there is no log file left to delete. */
static FRESULT delete_oldest(void)
{
    char dir[DATA_LOG_NAME_MAX];
    char name[DATA_LOG_NAME_MAX];
    char path[DATA_LOG_PATH_MAX];
    DIR d;
    FILINFO fno;
    bool found = false;
    uint16_t lowest = 0u;

    FRESULT fr = oldest_dir(dir);
    if (fr != FR_OK) {
        return fr;
    }
    fr = f_opendir(&d, dir);
    if (fr != FR_OK) {
        return fr;
    }
    while ((fr = f_readdir(&d, &fno)) == FR_OK && fno.fname[0] != '\0') {
        uint16_t n;
        if (data_log_parse_file(fno.fname, &n) && (!found || n < lowest)) {
            lowest = n;
            found = true;
        }
    }
    (void)f_closedir(&d);
    if (fr != FR_OK) {
        return fr;
    }
    if (!found) {
        /* Empty of log files. A directory holding other files is refused,
           and the clean-up stops there. */
        fr = f_unlink(dir);
        return fr == FR_OK ? FR_OK : FR_NO_FILE;
    }
    data_log_file_name(lowest, name);
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    fr = f_unlink(path);
    if (fr == FR_OK) {
        status.deleted++;
    }
    return fr;
}

/* Deletes the oldest files until DATA_LOG_FREE_PERCENT is free, or there
   are none left to delete (the file open is refused by FatFS): then the
   logger writes until the card is full. */
static FRESULT make_space(void)
{
    for (uint32_t i = 0; i < DATA_LOG_DELETES_MAX; i++) {
        DWORD freeClusters = 0u;
        FATFS *fs = NULL;
        FRESULT fr = f_getfree(USERPath, &freeClusters, &fs);

        if (fr != FR_OK) {
            return fr;
        }
        if (!data_log_card_needs_space((uint32_t)freeClusters, (uint32_t)(fs->n_fatent - 2u))) {
            return FR_OK;
        }
        fr = delete_oldest();
        if (fr == FR_NO_FILE) {
            return FR_OK;
        }
        if (fr != FR_OK) {
            return fr;
        }
    }
    return FR_OK;
}

/* ---- the card ---------------------------------------------------------- */

/* Closes and unmounts whatever is open, so the next try starts the card
   from scratch, and writes the one Event the incident is due. */
static void fail(bool mount, FRESULT fr, uint32_t now)
{
    if (fileOpen) {
        (void)f_close(&USERFile);
        fileOpen = false;
    }
    log_batch_init(&batch);
    (void)f_mount(NULL, USERPath, 0u);
    mounted = false;
    status.file[0] = '\0';
    status.last_result = (uint8_t)fr;

    const bool report = mount ? data_log_card_mount_failed(&card, now)
                              : data_log_card_write_failed(&card, now);
    if (report) {
        const event_data_log_card_t ev = {
            .state = (uint8_t)card.state,
            .result = (uint8_t)fr,
            .written = status.written,
            .dropped = data_log_dropped(),
        };
        uint8_t data[EVENT_DATA_SIZE];

        event_data_data_log_card(data, &ev);
        (void)events_post(EVENT_DATA_LOG_CARD, data);
    }
}

static void try_open(uint32_t now)
{
    /* The Event writer gives the boot number; it opens its log long before
       STARTUP ends, so this waits a poll at most. */
    if (!events_boot_number(&boot)) {
        return;
    }
    if (!mounted) {
        const FRESULT fr = f_mount(&USERFatFs, USERPath, 1u);
        if (fr != FR_OK) {
            fail(true, fr, now);
            return;
        }
        mounted = true;
    }

    FRESULT fr = make_space();
    if (fr == FR_OK) {
        fr = open_file(now);
    }
    if (fr != FR_OK) {
        fail(false, fr, now);
    }
}

static void roll(uint32_t now)
{
    FRESULT fr = close_file();

    if (fr == FR_OK) {
        fr = make_space();
    }
    if (fr == FR_OK) {
        fr = open_file(now);
    }
    if (fr != FR_OK) {
        fail(false, fr, now);
    }
}

static void sync(uint32_t now)
{
    FRESULT fr = write_batch(false);

    if (fr == FR_OK) {
        fr = f_sync(&USERFile);
    }
    if (fr != FR_OK) {
        fail(false, fr, now);
        return;
    }
    data_log_card_synced(&card, now);
}

/* Empties one ring into the file, or counts what it could not write. */
static void drain(record_ring_t *ring, uint32_t now)
{
    record_head_t head;

    if (ring == NULL) {
        return;
    }
    while (record_ring_pop(ring, &head, payload, sizeof payload)) {
        if (card.state != DATA_LOG_LOGGING) {
            status.not_written++;
            continue;
        }
        const size_t n = data_log_record_encode(encoded, sizeof encoded, &head, payload);
        const FRESULT fr = put(encoded, n);
        if (fr != FR_OK) {
            fail(false, fr, now);
            status.not_written++;
            continue;
        }
        status.written++;
    }
}

/* True once the Control step has left STARTUP. */
static bool startup_over(void)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    return snap.running && snap.state != def->startup_state;
}

void dataLogTask(void *argument)
{
    (void)argument; /* the def came with data_log_init() */

    data_log_card_init(&card);
    log_batch_init(&batch);
    publish_status();

    for (;;) {
        osDelay(DATA_LOG_POLL_MS);
        const uint32_t now = osKernelGetTickCount();

        /* Until STARTUP ends the rings fill and are not read: the start's
           Records and Events wait for the card. */
        if (!card.started) {
            if (!startup_over()) {
                continue;
            }
            data_log_card_start(&card);
        }

        if (data_log_card_try_due(&card, now)) {
            try_open(now);
        }
        if (data_log_card_roll_due(&card, now)) {
            roll(now);
        }
        /* Events first: the rarest and the most wanted. */
        drain(data_log_ring(DATA_LOG_RING_EVENT), now);
        for (size_t i = 0; i < DATA_LOG_RING_COUNT; i++) {
            if (i != DATA_LOG_RING_EVENT) {
                drain(data_log_ring((data_log_ring_id_t)i), now);
            }
        }
        if (data_log_card_sync_due(&card, now)) {
            sync(now);
        }
        publish_status();
    }
}

/* ---- the read-out ------------------------------------------------------ */

static const char *state_name(data_log_state_t state)
{
    switch (state) {
    case DATA_LOG_WAITING: return "waiting for the end of STARTUP";
    case DATA_LOG_MISSING: return "MISSING (no card, retried every 5 s)";
    case DATA_LOG_LOGGING: return "LOGGING";
    case DATA_LOG_ERROR:   return "ERROR (retried every 5 s)";
    default:               return "?";
    }
}

int data_log_cmd(int argc, char **argv)
{
    (void)argv;
    data_log_status_t s;

    if (argc != 1) {
        shell_printf("usage: datalog\r\n");
        return 0;
    }
    if (statusMutex == NULL || data_log_ring(DATA_LOG_RING_STEP) == NULL) {
        shell_printf("datalog: not running\r\n");
        return 0;
    }
    (void)osMutexAcquire(statusMutex, osWaitForever);
    s = shared;
    (void)osMutexRelease(statusMutex);

    static const char *const ringNames[DATA_LOG_RING_COUNT] = {
        [DATA_LOG_RING_STEP] = "step",
        [DATA_LOG_RING_EVENT] = "event",
        [DATA_LOG_RING_IMU] = "imu",
        [DATA_LOG_RING_DAQ_RX] = "daq rx",
        [DATA_LOG_RING_DAQ_TX] = "daq tx",
        [DATA_LOG_RING_SNAPSHOT] = "snapshot",
    };

    shell_printf("datalog: card %s\r\n", state_name(s.state));
    shell_printf("datalog: file %s\r\n", s.file[0] != '\0' ? s.file : "(none)");
    shell_printf("datalog: records written %lu, dropped %lu, not written %lu\r\n",
                 (unsigned long)s.written, (unsigned long)data_log_dropped(),
                 (unsigned long)s.not_written);
    for (size_t i = 0; i < DATA_LOG_RING_COUNT; i++) {
        const record_ring_t *ring = data_log_ring((data_log_ring_id_t)i);
        if (ring != NULL) {
            shell_printf("datalog:   %-6s %3lu deep, dropped %lu\r\n", ringNames[i],
                         (unsigned long)record_ring_depth(ring),
                         (unsigned long)record_ring_dropped(ring));
        }
    }
    shell_printf("datalog: files opened %lu, deleted for space %lu, last FatFS error %u\r\n",
                 (unsigned long)s.files, (unsigned long)s.deleted, (unsigned)s.last_result);
    return 0;
}
