/* data_log_card.c — the Data log's card handling, see data_log_card.h. */

#include "data_log_card.h"

#include <string.h>

#include "can_daq.h"

void data_log_card_init(data_log_card_t *c)
{
    memset(c, 0, sizeof *c);
    c->state = DATA_LOG_WAITING;
}

void data_log_card_start(data_log_card_t *c)
{
    c->started = true;
}

bool data_log_card_try_due(const data_log_card_t *c, uint32_t now_ms)
{
    switch (c->state) {
    case DATA_LOG_WAITING:
        return c->started;
    case DATA_LOG_MISSING:
    case DATA_LOG_ERROR:
        return now_ms - c->tried_ms >= DATA_LOG_RETRY_MS;
    default:
        return false;
    }
}

void data_log_card_opened(data_log_card_t *c, uint32_t now_ms)
{
    c->state = DATA_LOG_LOGGING;
    c->opened_ms = now_ms;
    c->synced_ms = now_ms;
}

/* One Event per incident: only the way out of logging, or out of the
   first try, reports. */
static bool fail(data_log_card_t *c, data_log_state_t to, uint32_t now_ms)
{
    const bool report = c->state == DATA_LOG_WAITING || c->state == DATA_LOG_LOGGING;

    c->state = to;
    c->tried_ms = now_ms;
    return report;
}

bool data_log_card_mount_failed(data_log_card_t *c, uint32_t now_ms)
{
    return fail(c, DATA_LOG_MISSING, now_ms);
}

bool data_log_card_write_failed(data_log_card_t *c, uint32_t now_ms)
{
    return fail(c, DATA_LOG_ERROR, now_ms);
}

bool data_log_card_roll_due(const data_log_card_t *c, uint32_t now_ms)
{
    return c->state == DATA_LOG_LOGGING && now_ms - c->opened_ms >= DATA_LOG_ROLL_MS;
}

bool data_log_card_sync_due(const data_log_card_t *c, uint32_t now_ms)
{
    return c->state == DATA_LOG_LOGGING && now_ms - c->synced_ms >= DATA_LOG_SYNC_MS;
}

void data_log_card_synced(data_log_card_t *c, uint32_t now_ms)
{
    c->synced_ms = now_ms;
}

uint8_t data_log_card_choice(data_log_state_t state)
{
    /* The choice values are the same in both images' frames (can/daq.dbc). */
    switch (state) {
    case DATA_LOG_LOGGING:
        return CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_LOGGING_CHOICE;
    case DATA_LOG_ERROR:
        return CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_ERROR_CHOICE;
    default:
        return CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_MISSING_CHOICE;
    }
}

bool data_log_card_needs_space(uint32_t free_units, uint32_t total_units)
{
    return (uint64_t)free_units * 100u < (uint64_t)total_units * DATA_LOG_FREE_PERCENT;
}

/* prefix, then value as exactly digits decimal digits, then suffix. */
static void format(char *out, char prefix, uint32_t value, uint32_t digits, const char *suffix)
{
    out[0] = prefix;
    for (uint32_t i = digits; i > 0u; i--) {
        out[i] = (char)('0' + value % 10u);
        value /= 10u;
    }
    strcpy(&out[digits + 1u], suffix);
}

/* The value of name if it is prefix, then exactly digits decimal digits,
   then suffix, and at most max. */
static bool parse(const char *name, char prefix, uint32_t digits, const char *suffix,
                  uint32_t max, uint32_t *value)
{
    uint32_t v = 0u;

    if (name[0] != prefix) {
        return false;
    }
    for (uint32_t i = 1u; i <= digits; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(name[i] - '0');
    }
    if (strcmp(&name[digits + 1u], suffix) != 0 || v > max) {
        return false;
    }
    *value = v;
    return true;
}

void data_log_dir_name(uint16_t boot, char out[DATA_LOG_NAME_MAX])
{
    format(out, 'B', boot, 5u, "");
}

bool data_log_parse_dir(const char *name, uint16_t *boot)
{
    uint32_t v;

    if (!parse(name, 'B', 5u, "", 0xFFFFu, &v)) {
        return false;
    }
    *boot = (uint16_t)v;
    return true;
}

void data_log_file_name(uint16_t file, char out[DATA_LOG_NAME_MAX])
{
    format(out, 'L', file, 4u, ".TDL");
}

bool data_log_parse_file(const char *name, uint16_t *file)
{
    uint32_t v;

    if (!parse(name, 'L', 4u, ".TDL", 9999u, &v)) {
        return false;
    }
    *file = (uint16_t)v;
    return true;
}

bool data_log_boot_older(uint16_t a, uint16_t b, uint16_t current)
{
    return (uint16_t)(current - a) > (uint16_t)(current - b);
}
