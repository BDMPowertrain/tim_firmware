/*
 * What the SD logger does with the card (ADR 0021, issue 53), as decisions
 * from values and the time; the logger task (data_log.h) does the FatFS
 * calls they ask for.
 *
 * States: WAITING until the end of STARTUP, then the first mount is tried
 * at once. LOGGING while a file is open. A mount that fails (no card, or
 * not FAT) is MISSING; a write, sync or open that fails is ERROR. Either
 * way the file is closed and the mount is retried every
 * DATA_LOG_RETRY_MS into a new file. One Event is written per incident:
 * when logging (or the first try) turns into MISSING or ERROR, not again
 * while the retries keep failing. No state is a fault (ADR 0021).
 *
 * Files: one directory per boot number ("B00042"), a new file each boot
 * and each retry ("L0001.TDL", the next number free in that directory),
 * rolled every DATA_LOG_ROLL_MS and synced every DATA_LOG_SYNC_MS. When
 * less than DATA_LOG_FREE_PERCENT of the card is free the oldest files go
 * first: the boot furthest behind this one, its lowest file number first.
 *
 * Names are 8.3 (FatFS is built without long names).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef DATA_LOG_CARD_H
#define DATA_LOG_CARD_H

#include <stdbool.h>
#include <stdint.h>

#define DATA_LOG_RETRY_MS     5000u
#define DATA_LOG_ROLL_MS      600000u /* 10 minutes */
#define DATA_LOG_SYNC_MS      1000u
#define DATA_LOG_FREE_PERCENT 10u

/* Room for an 8.3 name and its NUL. */
#define DATA_LOG_NAME_MAX 13u

typedef enum {
    DATA_LOG_WAITING = 0, /* STARTUP has not ended */
    DATA_LOG_MISSING = 1, /* no card, or it will not mount */
    DATA_LOG_LOGGING = 2, /* a file is open */
    DATA_LOG_ERROR   = 3, /* a write, sync or open failed */
} data_log_state_t;

typedef struct {
    data_log_state_t state;
    bool     started;   /* STARTUP has ended */
    uint32_t tried_ms;  /* the last failed try */
    uint32_t opened_ms; /* the open file's */
    uint32_t synced_ms; /* the open file's last sync */
} data_log_card_t;

/* WAITING. */
void data_log_card_init(data_log_card_t *c);

/* STARTUP has ended: the first mount is due at once. */
void data_log_card_start(data_log_card_t *c);

/* A mount (and a new file) should be tried now. */
bool data_log_card_try_due(const data_log_card_t *c, uint32_t now_ms);

/* A file is open. */
void data_log_card_opened(data_log_card_t *c, uint32_t now_ms);

/* The mount failed. True if an Event is due. */
bool data_log_card_mount_failed(data_log_card_t *c, uint32_t now_ms);

/* A write, sync or open failed; the file is closed. True if an Event is due. */
bool data_log_card_write_failed(data_log_card_t *c, uint32_t now_ms);

/* The open file has run DATA_LOG_ROLL_MS: close it and open the next. */
bool data_log_card_roll_due(const data_log_card_t *c, uint32_t now_ms);

/* The open file is due a sync. */
bool data_log_card_sync_due(const data_log_card_t *c, uint32_t now_ms);
void data_log_card_synced(data_log_card_t *c, uint32_t now_ms);

/* The DBC's CardState choice for state (can/daq.dbc): WAITING reports as
   MISSING. */
uint8_t data_log_card_choice(data_log_state_t state);

/* Less than DATA_LOG_FREE_PERCENT of the card is free. Any unit, the same
   for both (FatFS clusters). */
bool data_log_card_needs_space(uint32_t free_units, uint32_t total_units);

/* "B00042" for boot 42, and back. */
void data_log_dir_name(uint16_t boot, char out[DATA_LOG_NAME_MAX]);
bool data_log_parse_dir(const char *name, uint16_t *boot);

/* "L0003.TDL" for file 3, and back. */
void data_log_file_name(uint16_t file, char out[DATA_LOG_NAME_MAX]);
bool data_log_parse_file(const char *name, uint16_t *file);

/* Boot a is older than boot b, both counted back from this boot (the boot
   number is 16 bits and wraps). */
bool data_log_boot_older(uint16_t a, uint16_t b, uint16_t current);

#endif /* DATA_LOG_CARD_H */
