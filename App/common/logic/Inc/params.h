/*
 * Parameters (ADR 0014, 0015): the values a person sets at the car, kept in
 * two full copies in bank 2 so that a power loss during a save leaves the
 * previous set intact.
 *
 * Each image has one parameter table (param_table_t): a row per parameter
 * gives its stored ID, name, type, default, minimum and maximum. A row's
 * position in the table is its index, which code uses to read the value;
 * the ID is what is stored, and it never changes once used.
 *
 * A copy, little-endian, PARAMS_COPY_SIZE bytes at PARAMS_COPY_A_ADDR or
 * PARAMS_COPY_B_ADDR:
 *   0  marker        PARAMS_MARKER
 *   4  image         the table's image, PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS
 *   8  save counter  one more than the newest copy at the time of the save
 *   12 length        bytes of pairs that follow the header
 *   16 reserved      0
 *   20 CRC-32        of bytes 0 to 19 and the pairs
 *   24 pairs         8 bytes each: ID (u16), 0 (u16), value (i32)
 *
 * Load (once, at start-up) takes the newest copy, by save counter, whose
 * marker, length and CRC are good and whose image is this table's. From it
 * an unknown ID is ignored, a missing ID gets its default and an
 * out-of-range value gets its default and is reported. With no such copy
 * every parameter gets its default. A copy whose read raises an ECC error
 * (a torn write) is skipped like a bad one.
 *
 * The values loaded are the ones in force until the next reset. A set
 * changes only the pending value; a save writes the pending values over the
 * older copy (erase, write, read back, compare) and changes nothing in
 * force.
 *
 * Without flash (the part is not dual-bank) there is no read and no write:
 * every parameter gets its default and every save is refused.
 *
 * A save writes only the IDs in this table, so an ID that a newer image
 * stored and this one does not know is lost on the next save.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_if.h"

#define PARAMS_COPY_A_ADDR 0x08040000u
#define PARAMS_COPY_B_ADDR 0x08041000u
#define PARAMS_COPY_SIZE   0x1000u
#define PARAMS_HEADER_SIZE 24u
#define PARAMS_PAIR_SIZE   8u
#define PARAMS_MAX_COUNT   ((PARAMS_COPY_SIZE - PARAMS_HEADER_SIZE) / PARAMS_PAIR_SIZE)
#define PARAMS_MARKER      0x4D524150u /* "PARM" */

#define PARAMS_IMAGE_VCU 1u
#define PARAMS_IMAGE_BMS 2u

/* What a value is, for its bounds and for the shell. Every value is held
   as an int32_t; a table's minimum and maximum must fit its type. */
typedef enum {
    PARAM_BOOL,
    PARAM_U8,
    PARAM_U16,
    PARAM_I16,
    PARAM_I32,
} param_type_t;

typedef struct {
    uint16_t     id;   /* stored; never reused for another meaning */
    const char  *name; /* for the shell */
    param_type_t type;
    int32_t      def;
    int32_t      min;
    int32_t      max;
} param_def_t;

typedef struct {
    uint32_t           image; /* PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS */
    const param_def_t *defs;
    size_t             count;
} param_table_t;

/* Where a value in force came from. */
typedef enum {
    PARAM_FROM_COPY,            /* the copy loaded */
    PARAM_DEFAULT_NO_COPY,      /* no valid copy, or no flash */
    PARAM_DEFAULT_MISSING,      /* the copy loaded has no such ID */
    PARAM_DEFAULT_OUT_OF_RANGE, /* the copy loaded holds `rejected` */
} param_origin_t;

typedef struct {
    int32_t        value;    /* in force since start-up */
    int32_t        pending;  /* what the next save writes */
    int32_t        rejected; /* the stored value, if PARAM_DEFAULT_OUT_OF_RANGE */
    param_origin_t origin;
} param_slot_t;

/* What load found in one copy. */
typedef enum {
    PARAMS_COPY_GOOD,
    PARAMS_COPY_EMPTY,       /* erased marker: never written */
    PARAMS_COPY_BAD_HEADER,  /* marker or length wrong */
    PARAMS_COPY_BAD_CRC,
    PARAMS_COPY_WRONG_IMAGE, /* good, but the other image wrote it */
    PARAMS_COPY_READ_FAULT,  /* an ECC error: a torn write */
    PARAMS_COPY_NOT_READ,    /* no flash */
} params_copy_status_t;

typedef enum {
    PARAMS_LOADED,    /* from a valid copy */
    PARAMS_DEFAULTED, /* no valid copy: "parameters defaulted" */
    PARAMS_NO_FLASH,  /* not dual-bank: defaults, no flash read or write */
} params_source_t;

typedef struct {
    const param_table_t *table;
    const flash_if_t    *flash; /* NULL: no flash */
    param_slot_t        *slots; /* table->count of them, by index */

    params_source_t      source;
    params_copy_status_t copy_status[2]; /* A, B */
    uint32_t             copy_counter[2]; /* save counter, if GOOD or WRONG_IMAGE */
    uint8_t              copy_loaded;     /* 0 A, 1 B, if PARAMS_LOADED */
    uint16_t             unknown;         /* IDs ignored in the copy loaded */
    uint16_t             missing;         /* parameters defaulted as missing */
    uint16_t             out_of_range;    /* parameters defaulted as out of range */

    uint32_t             saves;           /* saves completed since start-up */
    uint32_t             last_save_counter;
} params_t;

typedef enum {
    PARAMS_SET_OK,
    PARAMS_SET_OUT_OF_RANGE,
    PARAMS_SET_NO_SUCH_INDEX,
} params_set_result_t;

typedef enum {
    PARAMS_SAVE_OK,
    PARAMS_SAVE_NO_FLASH,
    PARAMS_SAVE_ERASE_FAILED,
    PARAMS_SAVE_WRITE_FAILED,
    PARAMS_SAVE_VERIFY_FAILED, /* read back differs or faults */
} params_save_result_t;

/* True if the table is usable: count at most PARAMS_MAX_COUNT, IDs unique,
   min <= default <= max, and min and max inside the type. */
bool params_table_ok(const param_table_t *table);

/* Loads the values in force. flash is NULL when the part is not dual-bank.
   slots has table->count entries. The table must pass params_table_ok(). */
void params_load(params_t *p, const param_table_t *table, const flash_if_t *flash,
                 param_slot_t *slots);

/* "Parameters defaulted" (ADR 0015): no valid copy was loaded, or a value
   in it was missing or out of range. */
bool params_defaulted(const params_t *p);

/* The value in force of the parameter at index. */
int32_t params_value(const params_t *p, size_t index);

/* The index of the parameter called name, or table->count if none. */
size_t params_find(const params_t *p, const char *name);

/* Sets the pending value of the parameter at index, if inside its bounds. */
params_set_result_t params_set(params_t *p, size_t index, int32_t value);

/* Writes every pending value over the older copy. Nothing in force
   changes. */
params_save_result_t params_save(params_t *p);

#endif /* PARAMS_H */
