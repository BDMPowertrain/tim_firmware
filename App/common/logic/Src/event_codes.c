/* event_codes.c — see event_codes.h. */

#include "event_codes.h"

#include <string.h>

static void put_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
    put_u16(b, (uint16_t)v);
    put_u16(&b[2], (uint16_t)(v >> 16));
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

#define PEDAL_CHECK_DIRTY 0x01u

const char *event_code_name(uint16_t code)
{
    switch (code) {
    case EVENT_BOOT:               return "boot";
    case EVENT_BOOT_CRASH:         return "Crash record";
    case EVENT_BOOT_BREADCRUMB:    return "Breadcrumb";
    case EVENT_STORAGE_REPAIRED:   return "storage repaired";
    case EVENT_EVENTS_DROPPED:     return "events dropped";
    case EVENT_NOT_DUAL_BANK:      return "not dual-bank";
    case EVENT_PARAMS_DEFAULTED:   return "parameters defaulted";
    case EVENT_PARAM_OUT_OF_RANGE: return "parameter out of range";
    case EVENT_PARAM_SAVE:         return "parameter save";
    case EVENT_OVERRUN:            return "overrun";
    case EVENT_BUS_OFF:            return "bus-off";
    case EVENT_ERROR_PASSIVE:      return "error-passive";
    case EVENT_BUS_OFF_LATCHED:    return "bus-off latched";
    case EVENT_BUS_OFF_UNLATCHED:  return "bus-off latch cleared";
    case EVENT_MAILBOX_TIMEOUT:    return "mailbox timeout";
    case EVENT_VCU_STATE:          return "VCU state";
    case EVENT_TORQUE_CUT:         return "torque cut";
    case EVENT_PEDAL_CHECK:        return "pedal check passed";
    case EVENT_PEDAL_CHECK_FAILED: return "pedal check failed";
    case EVENT_NOT_CALIBRATED:     return "not calibrated";
    case EVENT_DATA_LOG_CARD:      return "data log card";
    default:                       return "?";
    }
}

/* What follows the boot Event: nothing after a Power-on reset, which
   clears both records (reset_record.h). */
static bool crash_follows(const reset_report_t *r)
{
    return !reset_cause_is_power_on(r->cause) && r->layout_found && r->crash.cause != CRASH_NONE;
}

static bool crumb_follows(const reset_report_t *r)
{
    return !reset_cause_is_power_on(r->cause) && r->layout_found
           && r->breadcrumb.phase != BREADCRUMB_PHASE_NONE;
}

void event_data_boot(uint8_t data[EVENT_DATA_SIZE], const event_boot_t *boot)
{
    const reset_report_t *r = boot->reset;
    uint8_t flags = 0u;

    flags |= boot->dirty ? EVENT_BOOT_DIRTY : 0u;
    flags |= crash_follows(r) ? EVENT_BOOT_CRASH_NEXT : 0u;
    flags |= crumb_follows(r) ? EVENT_BOOT_CRUMB_NEXT : 0u;

    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = boot->image;
    data[1] = (uint8_t)r->cause;
    data[2] = flags;
    put_u32(&data[4], boot->build_id);
    put_u32(&data[8], r->non_power_on_resets);
}

void event_data_crash(uint8_t data[EVENT_DATA_SIZE], const crash_record_t *crash)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = (uint8_t)crash->cause;
    put_u32(&data[4], crash->address);
    /* Up to 8 characters, NUL-padded: strnlen is not in C11. */
    for (uint32_t i = 0; i < CRASH_TASK_CHARS && crash->task[i] != '\0'; i++) {
        data[8u + i] = (uint8_t)crash->task[i];
    }
}

void event_data_breadcrumb(uint8_t data[EVENT_DATA_SIZE], const breadcrumb_t *crumb)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u32(&data[0], crumb->step_count);
    put_u32(&data[4], crumb->overruns);
    data[8] = crumb->phase;
}

void event_data_repair(uint8_t data[EVENT_DATA_SIZE], const event_log_repair_t *repair,
                       uint32_t repairs)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = repair->page;
    data[1] = repair->kept;
    data[2] = repair->header ? 1u : 0u;
    data[3] = repair->erased ? 1u : 0u;
    put_u32(&data[4], repairs);
}

void event_data_dropped(uint8_t data[EVENT_DATA_SIZE], uint32_t n)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u32(&data[0], n);
}

void event_data_params_defaulted(uint8_t data[EVENT_DATA_SIZE], const params_t *p)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = (uint8_t)p->source;
    data[1] = (uint8_t)p->copy_status[0];
    data[2] = (uint8_t)p->copy_status[1];
    put_u16(&data[4], p->missing);
    put_u16(&data[6], p->out_of_range);
    put_u16(&data[8], p->unknown);
}

void event_data_param_out_of_range(uint8_t data[EVENT_DATA_SIZE], uint16_t id, int32_t stored,
                                   int32_t def)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u16(&data[0], id);
    put_u32(&data[4], (uint32_t)stored);
    put_u32(&data[8], (uint32_t)def);
}

void event_data_param_save(uint8_t data[EVENT_DATA_SIZE], uint8_t result, uint32_t counter)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = result;
    put_u32(&data[4], counter);
}

void event_data_overrun(uint8_t data[EVENT_DATA_SIZE], const overrun_event_t *overrun)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u32(&data[0], overrun->step_us);
    put_u32(&data[4], overrun->slowest_phase_us);
    data[8] = overrun->slowest_phase;
}

void event_data_bus(uint8_t data[EVENT_DATA_SIZE], const event_bus_t *bus)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u32(&data[0], bus->bus_offs);
    put_u32(&data[4], bus->error_passives);
    data[8] = bus->tec;
    data[9] = bus->rec;
    data[10] = bus->latched ? 1u : 0u;
}

void event_data_mailbox_timeout(uint8_t data[EVENT_DATA_SIZE], uint32_t frame_id,
                                uint32_t timeouts, uint8_t row)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u32(&data[0], frame_id);
    put_u32(&data[4], timeouts);
    data[8] = row;
}

void event_data_vcu_state(uint8_t data[EVENT_DATA_SIZE], const event_vcu_state_t *state)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = state->from;
    data[1] = state->to;
    data[2] = state->cause;
    put_u16(&data[4], (uint16_t)state->bus_dv);
    put_u16(&data[6], state->pack_dv);
    put_u16(&data[8], state->causes);
}

void event_data_torque_cut(uint8_t data[EVENT_DATA_SIZE], const event_torque_cut_t *cut)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = cut->cause;
    data[1] = cut->started ? 1u : 0u;
    data[2] = cut->state;
    put_u16(&data[4], cut->travel[0]);
    put_u16(&data[6], cut->travel[1]);
    data[8] = cut->apps_status[0];
    data[9] = cut->apps_status[1];
    data[10] = cut->brake_status[0];
    data[11] = cut->brake_status[1];
}

void event_data_data_log_card(uint8_t data[EVENT_DATA_SIZE], const event_data_log_card_t *card)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = card->state;
    data[1] = card->result;
    put_u32(&data[4], card->written);
    put_u32(&data[8], card->dropped);
}

void event_data_pedal_check(uint8_t data[EVENT_DATA_SIZE], uint32_t build_id,
                            uint32_t save_counter, uint32_t calibration, bool dirty)
{
    memset(data, 0, EVENT_DATA_SIZE);
    put_u32(&data[0], build_id);
    put_u32(&data[4], save_counter);
    data[8] = dirty ? PEDAL_CHECK_DIRTY : 0u;
    put_u32(&data[12], calibration);
}

bool event_pedal_check_matches(const event_t *ev, const void *key)
{
    const event_pedal_check_key_t *k = (const event_pedal_check_key_t *)key;

    if (k->dirty || k->build_id == 0u || ev->code != EVENT_PEDAL_CHECK) {
        return false;
    }
    return get_u32(&ev->data[0]) == k->build_id && get_u32(&ev->data[4]) == k->save_counter &&
           get_u32(&ev->data[12]) == k->calibration && (ev->data[8] & PEDAL_CHECK_DIRTY) == 0u;
}

void event_data_pedal_check_failed(uint8_t data[EVENT_DATA_SIZE], uint8_t reason, uint8_t phase,
                                   const uint16_t travel[2])
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = reason;
    data[1] = phase;
    put_u16(&data[4], travel[0]);
    put_u16(&data[6], travel[1]);
}

void event_data_not_calibrated(uint8_t data[EVENT_DATA_SIZE], uint8_t reason, uint8_t sensor,
                               uint16_t stored_revision, uint16_t image_revision)
{
    memset(data, 0, EVENT_DATA_SIZE);
    data[0] = reason;
    data[1] = sensor;
    put_u16(&data[2], stored_revision);
    put_u16(&data[4], image_revision);
}
