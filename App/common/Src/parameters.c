/* parameters.c — the running image's Parameters, see parameters.h.

   Until the Event log exists, what load found is printed at start-up and
   shown by `param dump`; it becomes the "parameters defaulted", "parameter
   out of range", "not dual-bank" and "parameter save" Events then. Command
   names and output are provisional (#28). */

#include "parameters.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

bool parameters_init(const param_table_t *table, parameters_save_hook_t save_hook)
{
    if (!params_table_ok(table) || table->count > PARAMETERS_MAX) {
        shell_printf("params: table unusable, nothing loaded\r\n");
        return false;
    }

    saveHook = save_hook;
    params_load(&store, table, flash_bank2_get(), slots);
    ready = true;

    /* At most two lines: this runs before printTask exists, and a full
       print queue would block start-up for good. `param dump` names each
       value defaulted. */
    print_source();
    if (store.out_of_range > 0u) {
        shell_printf("params: %u out of range, defaulted (param dump)\r\n",
                     (unsigned)store.out_of_range);
    }
    return true;
}

int32_t parameters_value(size_t index)
{
    return ready ? params_value(&store, index) : 0;
}

bool parameters_defaulted(void)
{
    return !ready || params_defaulted(&store);
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

static void param_save(void)
{
    const char *refusal = saveHook != NULL ? saveHook() : NULL;

    if (refusal != NULL) {
        shell_printf("param: save refused: %s\r\n", refusal);
        return;
    }

    switch (params_save(&store)) {
    case PARAMS_SAVE_OK:
        shell_printf("param: saved (save %lu), in force after a reset\r\n",
                     (unsigned long)store.last_save_counter);
        break;
    case PARAMS_SAVE_NO_FLASH:
        shell_printf("param: save refused: not dual-bank\r\n");
        break;
    case PARAMS_SAVE_ERASE_FAILED:
        shell_printf("param: save failed: erase\r\n");
        break;
    case PARAMS_SAVE_WRITE_FAILED:
        shell_printf("param: save failed: write\r\n");
        break;
    case PARAMS_SAVE_VERIFY_FAILED:
        shell_printf("param: save failed: read back differs\r\n");
        break;
    default:
        break;
    }
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
