/* params.c — Parameter storage over the flash interface, see params.h. */

#include "params.h"

#include <string.h>

#include "crc32.h"

#define COPY_COUNT 2u

static const uint32_t copyAddr[COPY_COUNT] = { PARAMS_COPY_A_ADDR, PARAMS_COPY_B_ADDR };

/* Header field offsets. */
#define OFF_MARKER   0u
#define OFF_IMAGE    4u
#define OFF_COUNTER  8u
#define OFF_LENGTH   12u
#define OFF_RESERVED 16u
#define OFF_CRC      20u

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* ---- the table --------------------------------------------------------- */

static void type_bounds(param_type_t type, int32_t *lo, int32_t *hi)
{
    switch (type) {
    case PARAM_BOOL: *lo = 0;         *hi = 1;          break;
    case PARAM_U8:   *lo = 0;         *hi = 255;        break;
    case PARAM_U16:  *lo = 0;         *hi = 65535;      break;
    case PARAM_I16:  *lo = -32768;    *hi = 32767;      break;
    case PARAM_I32:  *lo = INT32_MIN; *hi = INT32_MAX;  break;
    default:         *lo = 1;         *hi = 0;          break; /* nothing fits */
    }
}

bool params_table_ok(const param_table_t *table)
{
    if (table == NULL || table->count > PARAMS_MAX_COUNT || (table->count > 0u && table->defs == NULL)) {
        return false;
    }

    for (size_t i = 0; i < table->count; i++) {
        const param_def_t *d = &table->defs[i];
        int32_t lo;
        int32_t hi;

        type_bounds(d->type, &lo, &hi);
        if (d->name == NULL || d->min < lo || d->max > hi || d->min > d->def || d->def > d->max) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (table->defs[j].id == d->id) {
                return false;
            }
        }
    }
    return true;
}

static size_t index_of_id(const param_table_t *table, uint16_t id)
{
    for (size_t i = 0; i < table->count; i++) {
        if (table->defs[i].id == id) {
            return i;
        }
    }
    return table->count;
}

/* ---- load ------------------------------------------------------------- */

/* Checks one copy; on GOOD or WRONG_IMAGE, *counter is its save counter and
   *length the bytes of pairs. */
static params_copy_status_t check_copy(const flash_if_t *flash, uint32_t addr, uint32_t image,
                                       uint32_t *counter, uint32_t *length)
{
    uint8_t header[PARAMS_HEADER_SIZE];

    if (!flash->read(flash->ctx, addr, header, sizeof header)) {
        return PARAMS_COPY_READ_FAULT;
    }

    uint32_t marker = get_u32(&header[OFF_MARKER]);
    uint32_t len = get_u32(&header[OFF_LENGTH]);

    if (marker == 0xFFFFFFFFu) {
        return PARAMS_COPY_EMPTY;
    }
    if (marker != PARAMS_MARKER || len > PARAMS_COPY_SIZE - PARAMS_HEADER_SIZE
        || len % PARAMS_PAIR_SIZE != 0u) {
        return PARAMS_COPY_BAD_HEADER;
    }

    uint32_t crc = crc32_update(0u, header, OFF_CRC);

    for (uint32_t off = 0; off < len; off += PARAMS_PAIR_SIZE) {
        uint8_t pair[PARAMS_PAIR_SIZE];

        if (!flash->read(flash->ctx, addr + PARAMS_HEADER_SIZE + off, pair, sizeof pair)) {
            return PARAMS_COPY_READ_FAULT;
        }
        crc = crc32_update(crc, pair, sizeof pair);
    }
    if (crc != get_u32(&header[OFF_CRC])) {
        return PARAMS_COPY_BAD_CRC;
    }

    *counter = get_u32(&header[OFF_COUNTER]);
    *length = len;
    return get_u32(&header[OFF_IMAGE]) == image ? PARAMS_COPY_GOOD : PARAMS_COPY_WRONG_IMAGE;
}

/* The newest GOOD copy, or COPY_COUNT if none. Counters are compared by
   subtraction, so the wrap is harmless. */
static uint32_t newest_good(const params_t *p)
{
    bool a = p->copy_status[0] == PARAMS_COPY_GOOD;
    bool b = p->copy_status[1] == PARAMS_COPY_GOOD;

    if (a && b) {
        return (int32_t)(p->copy_counter[1] - p->copy_counter[0]) > 0 ? 1u : 0u;
    }
    if (a) {
        return 0u;
    }
    return b ? 1u : COPY_COUNT;
}

static void check_both(params_t *p, uint32_t length[COPY_COUNT])
{
    for (uint32_t c = 0; c < COPY_COUNT; c++) {
        p->copy_counter[c] = 0u;
        length[c] = 0u;
        p->copy_status[c] = check_copy(p->flash, copyAddr[c], p->table->image,
                                       &p->copy_counter[c], &length[c]);
    }
}

static void all_defaults(params_t *p)
{
    for (size_t i = 0; i < p->table->count; i++) {
        param_slot_t *s = &p->slots[i];

        s->value = p->table->defs[i].def;
        s->pending = s->value;
        s->rejected = 0;
        s->origin = PARAM_DEFAULT_NO_COPY;
    }
    p->unknown = 0u;
    p->missing = 0u;
    p->out_of_range = 0u;
}

/* Takes the values from copy c, already checked GOOD. False if a read now
   faults, which leaves the slots half filled. */
static bool apply_copy(params_t *p, uint32_t c, uint32_t length)
{
    for (size_t i = 0; i < p->table->count; i++) {
        p->slots[i].origin = PARAM_DEFAULT_MISSING;
    }

    for (uint32_t off = 0; off < length; off += PARAMS_PAIR_SIZE) {
        uint8_t pair[PARAMS_PAIR_SIZE];

        if (!p->flash->read(p->flash->ctx, copyAddr[c] + PARAMS_HEADER_SIZE + off, pair, sizeof pair)) {
            return false;
        }

        uint16_t id = (uint16_t)(pair[0] | (pair[1] << 8));
        int32_t value = (int32_t)get_u32(&pair[4]);
        size_t i = index_of_id(p->table, id);

        if (i == p->table->count) {
            p->unknown++;
            continue;
        }

        const param_def_t *d = &p->table->defs[i];
        param_slot_t *s = &p->slots[i];

        if (value >= d->min && value <= d->max) {
            s->value = value;
            s->origin = PARAM_FROM_COPY;
        } else {
            s->value = d->def;
            s->rejected = value;
            s->origin = PARAM_DEFAULT_OUT_OF_RANGE;
        }
        s->pending = s->value;
    }

    for (size_t i = 0; i < p->table->count; i++) {
        if (p->slots[i].origin == PARAM_DEFAULT_MISSING) {
            p->missing++;
        } else if (p->slots[i].origin == PARAM_DEFAULT_OUT_OF_RANGE) {
            p->out_of_range++;
        }
    }
    return true;
}

void params_load(params_t *p, const param_table_t *table, const flash_if_t *flash,
                 param_slot_t *slots)
{
    memset(p, 0, sizeof *p);
    p->table = table;
    p->flash = flash;
    p->slots = slots;
    all_defaults(p);

    if (flash == NULL) {
        p->source = PARAMS_NO_FLASH;
        p->copy_status[0] = PARAMS_COPY_NOT_READ;
        p->copy_status[1] = PARAMS_COPY_NOT_READ;
        return;
    }

    uint32_t length[COPY_COUNT];

    check_both(p, length);
    p->source = PARAMS_DEFAULTED;

    /* A copy that faults on this second read is skipped like a bad one, and
       the other good copy, if any, is tried. */
    for (uint32_t c = newest_good(p); c != COPY_COUNT; c = newest_good(p)) {
        if (apply_copy(p, c, length[c])) {
            p->source = PARAMS_LOADED;
            p->copy_loaded = (uint8_t)c;
            return;
        }
        p->copy_status[c] = PARAMS_COPY_READ_FAULT;
        all_defaults(p);
    }
}

bool params_defaulted(const params_t *p)
{
    return p->source != PARAMS_LOADED || p->missing > 0u || p->out_of_range > 0u;
}

int32_t params_value(const params_t *p, size_t index)
{
    return index < p->table->count ? p->slots[index].value : 0;
}

size_t params_find(const params_t *p, const char *name)
{
    for (size_t i = 0; i < p->table->count; i++) {
        if (strcmp(p->table->defs[i].name, name) == 0) {
            return i;
        }
    }
    return p->table->count;
}

params_set_result_t params_set(params_t *p, size_t index, int32_t value)
{
    if (index >= p->table->count) {
        return PARAMS_SET_NO_SUCH_INDEX;
    }

    const param_def_t *d = &p->table->defs[index];

    if (value < d->min || value > d->max) {
        return PARAMS_SET_OUT_OF_RANGE;
    }
    p->slots[index].pending = value;
    return PARAMS_SET_OK;
}

/* ---- save ------------------------------------------------------------- */

static void build_pair(const params_t *p, size_t i, uint8_t pair[PARAMS_PAIR_SIZE])
{
    uint16_t id = p->table->defs[i].id;

    pair[0] = (uint8_t)id;
    pair[1] = (uint8_t)(id >> 8);
    pair[2] = 0u;
    pair[3] = 0u;
    put_u32(&pair[4], (uint32_t)p->slots[i].pending);
}

static void build_header(const params_t *p, uint32_t counter, uint8_t header[PARAMS_HEADER_SIZE])
{
    uint32_t length = (uint32_t)p->table->count * PARAMS_PAIR_SIZE;

    put_u32(&header[OFF_MARKER], PARAMS_MARKER);
    put_u32(&header[OFF_IMAGE], p->table->image);
    put_u32(&header[OFF_COUNTER], counter);
    put_u32(&header[OFF_LENGTH], length);
    put_u32(&header[OFF_RESERVED], 0u);

    uint32_t crc = crc32_update(0u, header, OFF_CRC);

    for (size_t i = 0; i < p->table->count; i++) {
        uint8_t pair[PARAMS_PAIR_SIZE];

        build_pair(p, i, pair);
        crc = crc32_update(crc, pair, sizeof pair);
    }
    put_u32(&header[OFF_CRC], crc);
}

/* The double word at byte offset off of the copy being saved. */
static void copy_word(const params_t *p, const uint8_t header[PARAMS_HEADER_SIZE], uint32_t off,
                      uint8_t word[FLASH_IF_WORD_SIZE])
{
    if (off < PARAMS_HEADER_SIZE) {
        memcpy(word, &header[off], FLASH_IF_WORD_SIZE);
    } else {
        build_pair(p, (off - PARAMS_HEADER_SIZE) / PARAMS_PAIR_SIZE, word);
    }
}

params_save_result_t params_save(params_t *p)
{
    if (p->flash == NULL) {
        return PARAMS_SAVE_NO_FLASH;
    }

    /* Look again rather than trust start-up: a save since then has moved
       which copy is the older. Only a GOOD copy is kept; a bad one, or the
       other image's, is the one overwritten. */
    params_t now = *p;
    uint32_t length[COPY_COUNT];

    check_both(&now, length);

    uint32_t newest = newest_good(&now);
    uint32_t target = newest == 0u ? 1u : 0u;
    uint32_t counter = newest == COPY_COUNT ? 1u : now.copy_counter[newest] + 1u;
    uint32_t addr = copyAddr[target];
    uint32_t size = PARAMS_HEADER_SIZE + (uint32_t)p->table->count * PARAMS_PAIR_SIZE;
    uint8_t header[PARAMS_HEADER_SIZE];

    build_header(p, counter, header);

    for (uint32_t page = 0; page < PARAMS_COPY_SIZE; page += FLASH_IF_PAGE_SIZE) {
        if (!p->flash->erase(p->flash->ctx, addr + page)) {
            return PARAMS_SAVE_ERASE_FAILED;
        }
    }

    for (uint32_t off = 0; off < size; off += FLASH_IF_WORD_SIZE) {
        uint8_t word[FLASH_IF_WORD_SIZE];

        copy_word(p, header, off, word);
        if (!p->flash->write(p->flash->ctx, addr + off, word)) {
            return PARAMS_SAVE_WRITE_FAILED;
        }
    }

    for (uint32_t off = 0; off < size; off += FLASH_IF_WORD_SIZE) {
        uint8_t want[FLASH_IF_WORD_SIZE];
        uint8_t got[FLASH_IF_WORD_SIZE];

        copy_word(p, header, off, want);
        if (!p->flash->read(p->flash->ctx, addr + off, got, sizeof got)
            || memcmp(want, got, sizeof want) != 0) {
            return PARAMS_SAVE_VERIFY_FAILED;
        }
    }

    p->saves++;
    p->last_save_counter = counter;
    return PARAMS_SAVE_OK;
}
