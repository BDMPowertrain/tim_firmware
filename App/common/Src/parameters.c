/* parameters.c — the running image's Parameters, see parameters.h.

   Until the Event log exists, what load found is printed at start-up and
   shown by `param dump`; it becomes the "parameters defaulted", "parameter
   out of range", "not dual-bank" and "parameter save" Events then. Command
   names and output are provisional (#28). */

#include "parameters.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "event_codes.h"
#include "events.h"
#include "flash_bank2.h"
#include "print.h"

/* Enough for either image's table; parameters_init() refuses a bigger one. */
#define PARAMETERS_MAX 32u

static params_t store;
static param_slot_t slots[PARAMETERS_MAX];
static bool ready;
static parameters_save_hook_t saveHook;

static const char *copy_status_name(params_copy_status_t status)
{
    switch (status) {
    case PARAMS_COPY_GOOD:        return "good";
    case PARAMS_COPY_EMPTY:       return "empty";
    case PARAMS_COPY_BAD_HEADER:  return "bad header";
    case PARAMS_COPY_BAD_CRC:     return "bad CRC";
    case PARAMS_COPY_WRONG_IMAGE: return "other image";
    case PARAMS_COPY_READ_FAULT:  return "read fault";
    case PARAMS_COPY_NOT_READ:    return "not read";
    default:                      return "?";
    }
}

static const char *origin_name(param_origin_t origin)
{
    switch (origin) {
    case PARAM_FROM_COPY:            return "";
    case PARAM_DEFAULT_NO_COPY:      return "default";
    case PARAM_DEFAULT_MISSING:      return "default (missing)";
    case PARAM_DEFAULT_OUT_OF_RANGE: return "default (out of range)";
    default:                         return "?";
    }
}

static void print_source(void)
{
    switch (store.source) {
    case PARAMS_LOADED:
        shell_printf("params: loaded copy %c, save %lu\r\n", store.copy_loaded == 0u ? 'A' : 'B',
                     (unsigned long)store.copy_counter[store.copy_loaded]);
        break;
    case PARAMS_DEFAULTED:
        shell_printf("params: parameters defaulted, no valid copy\r\n");
        break;
    case PARAMS_NO_FLASH:
        shell_printf("params: parameters defaulted, not dual-bank: no flash read or write\r\n");
        break;
    default:
        break;
    }
}

/* Before the writer runs, into the queue: a table with more values out of
   range than the queue has room for loses the rest, counted as dropped. */
static void post_load_events(void)
{
    uint8_t data[EVENT_DATA_SIZE];

    if (params_defaulted(&store)) {
        event_data_params_defaulted(data, &store);
        (void)events_post(EVENT_PARAMS_DEFAULTED, data);
    }
    for (size_t i = 0; i < store.table->count; i++) {
        if (slots[i].origin == PARAM_DEFAULT_OUT_OF_RANGE) {
            event_data_param_out_of_range(data, store.table->defs[i].id, slots[i].rejected,
                                          slots[i].value);
            (void)events_post(EVENT_PARAM_OUT_OF_RANGE, data);
        }
    }
}

bool parameters_init(const param_table_t *table, parameters_save_hook_t save_hook)
{
    /* No printing here: this runs on initTask's 512-byte stack, which
       shell_printf()'s line buffer and vsnprintf would overflow. The shell
       prints the report when it starts. */
    if (!params_table_ok(table) || table->count > PARAMETERS_MAX) {
        return false;
    }

    saveHook = save_hook;
    params_load(&store, table, flash_bank2_get(), slots);
    ready = true;
    post_load_events();
    return true;
}

void parameters_print_report(void)
{
    if (!ready) {
        shell_printf("params: table unusable, nothing loaded\r\n");
        return;
    }
    print_source();
    if (store.out_of_range > 0u) {
        shell_printf("params: %u out of range, defaulted (param dump)\r\n",
                     (unsigned)store.out_of_range);
    }
}

int32_t parameters_value(size_t index)
{
    return ready ? params_value(&store, index) : 0;
}

bool parameters_from_copy(size_t index)
{
    return ready && index < store.table->count && slots[index].origin == PARAM_FROM_COPY;
}

uint32_t parameters_save_counter(void)
{
    if (!ready || store.source != PARAMS_LOADED) {
        return 0u;
    }
    return store.copy_counter[store.copy_loaded];
}

bool parameters_defaulted(void)
{
    return !ready || params_defaulted(&store);
}

/* ADR 0016: the calibration is a record, captured and never typed, so
   it is printed as one line, a default marked. */
static void print_calibration(void)
{
    bool any = false;
    bool pending = false;

    for (size_t i = 0; i < store.table->count; i++) {
        const param_def_t *d = &store.table->defs[i];

        if (!d->calibration) {
            continue;
        }
        if (!any) {
            shell_printf("calibration (captured, never typed):");
            any = true;
        }
        shell_printf(" %s=%ld%s", d->name, (long)slots[i].value,
                     slots[i].origin == PARAM_FROM_COPY ? "" : "(default)");
        pending = pending || slots[i].pending != slots[i].value;
    }
    if (any) {
        shell_printf("%s\r\n", pending ? "; a new one after save and reset" : "");
    }
}

static void param_dump(void)
{
    print_source();
    shell_printf("copy A %s, copy B %s; unknown %u, missing %u, out of range %u; saves %lu\r\n",
                 copy_status_name(store.copy_status[0]), copy_status_name(store.copy_status[1]),
                 (unsigned)store.unknown, (unsigned)store.missing, (unsigned)store.out_of_range,
                 (unsigned long)store.saves);

    for (size_t i = 0; i < store.table->count; i++) {
        const param_def_t *d = &store.table->defs[i];
        const param_slot_t *s = &slots[i];

        if (d->calibration) {
            continue; /* in the record below */
        }
        shell_printf("%-18s %8ld  [%ld..%ld] %s", d->name, (long)s->value, (long)d->min,
                     (long)d->max, origin_name(s->origin));
        if (s->origin == PARAM_DEFAULT_OUT_OF_RANGE) {
            shell_printf(", stored %ld", (long)s->rejected);
        }
        if (s->pending != s->value) {
            shell_printf("  -> %ld after save and reset", (long)s->pending);
        }
        shell_printf("\r\n");
    }
    print_calibration();
}

static void param_set(const char *name, const char *text)
{
    size_t i = params_find(&store, name);
    char *end;

    errno = 0;
    long value = strtol(text, &end, 0); /* long is 32 bits on the target */

    if (i == store.table->count) {
        shell_printf("param: no parameter '%s'\r\n", name);
        return;
    }
    if (store.table->defs[i].calibration) {
        shell_printf("param: %s is a calibration: captured by the image's guided command, "
                     "never typed\r\n", name);
        return;
    }
    if (*text == '\0' || *end != '\0' || errno == ERANGE) {
        shell_printf("param: bad value '%s'\r\n", text);
        return;
    }

    switch (params_set(&store, i, (int32_t)value)) {
    case PARAMS_SET_OK:
        shell_printf("param: %s = %ld after save and reset\r\n", name, value);
        break;
    case PARAMS_SET_OUT_OF_RANGE:
        shell_printf("param: %s must be %ld..%ld\r\n", name, (long)store.table->defs[i].min,
                     (long)store.table->defs[i].max);
        break;
    default:
        shell_printf("param: no parameter '%s'\r\n", name);
        break;
    }
}

/* Writes every pending value and posts the save Event. NULL once saved,
   otherwise why not. */
static const char *save_pending(void)
{
    const params_save_result_t result = params_save(&store);
    uint8_t data[EVENT_DATA_SIZE];

    event_data_param_save(data, (uint8_t)result,
                          result == PARAMS_SAVE_OK ? store.last_save_counter : 0u);
    (void)events_post(EVENT_PARAM_SAVE, data);

    switch (result) {
    case PARAMS_SAVE_OK:            return NULL;
    case PARAMS_SAVE_NO_FLASH:      return "refused: not dual-bank";
    case PARAMS_SAVE_ERASE_FAILED:  return "failed: erase";
    case PARAMS_SAVE_WRITE_FAILED:  return "failed: write";
    case PARAMS_SAVE_VERIFY_FAILED: return "failed: read back differs";
    default:                        return "failed";
    }
}

static void param_save(void)
{
    const char *refusal = saveHook != NULL ? saveHook() : NULL;

    if (refusal != NULL) {
        shell_printf("param: save refused: %s\r\n", refusal);
        return;
    }

    const char *failure = save_pending();

    if (failure == NULL) {
        shell_printf("param: saved (save %lu), in force after a reset\r\n",
                     (unsigned long)store.last_save_counter);
    } else {
        shell_printf("param: save %s\r\n", failure);
    }
}

const char *parameters_save_values(const size_t *index, const int32_t *value, size_t count)
{
    if (!ready) {
        return "no table loaded";
    }
    const char *refusal = saveHook != NULL ? saveHook() : NULL;

    if (refusal != NULL) {
        return refusal;
    }
    /* All in bounds first, so a refusal leaves nothing half set. */
    for (size_t i = 0; i < count; i++) {
        if (index[i] >= store.table->count || value[i] < store.table->defs[index[i]].min ||
            value[i] > store.table->defs[index[i]].max) {
            return "a value is outside its bounds";
        }
    }
    for (size_t i = 0; i < count; i++) {
        (void)params_set(&store, index[i], value[i]);
    }
    return save_pending();
}

#define PARAM_USAGE "usage: param [dump] | set <name> <value> | save\r\n"

int parameters_cmd(int argc, char **argv)
{
    if (!ready) {
        shell_printf("param: no table loaded\r\n");
        return 0;
    }

    if (argc < 2 || strcmp(argv[1], "dump") == 0) {
        param_dump();
    } else if (strcmp(argv[1], "set") == 0 && argc == 4) {
        param_set(argv[2], argv[3]);
    } else if (strcmp(argv[1], "save") == 0 && argc == 2) {
        param_save();
    } else {
        shell_printf(PARAM_USAGE);
    }
    return 0;
}
