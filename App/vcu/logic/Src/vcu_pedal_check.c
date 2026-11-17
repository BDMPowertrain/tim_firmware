/* vcu_pedal_check.c — see vcu_pedal_check.h. */

#include "vcu_pedal_check.h"

#include <string.h>

void vcu_pedal_check_reset(vcu_pedal_check_t *pc)
{
    memset(pc, 0, sizeof(*pc));
    pc->phase = VCU_PEDAL_CHECK_RELEASE;
}

static bool apps_ok(const vcu_sensors_t *sn)
{
    return sn->apps[0].status == VCU_SENSOR_OK && sn->apps[1].status == VCU_SENSOR_OK;
}

static bool brakes_ok(const vcu_sensors_t *sn)
{
    return sn->brake[0].status == VCU_SENSOR_OK && sn->brake[1].status == VCU_SENSOR_OK;
}

vcu_pedal_check_result_t vcu_pedal_check_step(vcu_pedal_check_t *pc, const vcu_sensors_t *sn,
                                              bool brake_engaged)
{
    const uint16_t a = sn->apps[0].travel;
    const uint16_t b = sn->apps[1].travel;
    const uint16_t apart = (a > b) ? (uint16_t)(a - b) : (uint16_t)(b - a);
    const bool released = apps_ok(sn) && a < VCU_PEDAL_CHECK_REST && b < VCU_PEDAL_CHECK_REST;
    const bool floored = apps_ok(sn) && a > VCU_PEDAL_CHECK_FLOOR && b > VCU_PEDAL_CHECK_FLOOR;

    if (pc->phase == VCU_PEDAL_CHECK_DONE && pc->brake_engaged) {
        return VCU_PEDAL_CHECK_NONE; /* passed already */
    }
    if (pc->phase != VCU_PEDAL_CHECK_RELEASE) {
        vcu_pedal_check_result_t fail = VCU_PEDAL_CHECK_NONE;

        if (!apps_ok(sn) || !brakes_ok(sn)) {
            fail = VCU_PEDAL_CHECK_FAIL_SENSOR;
        } else if (apart > VCU_PEDAL_CHECK_AGREE) {
            fail = VCU_PEDAL_CHECK_FAIL_APART;
        }
        if (fail != VCU_PEDAL_CHECK_NONE) {
            vcu_pedal_check_reset(pc);
            return fail;
        }
    }

    switch (pc->phase) {
    case VCU_PEDAL_CHECK_RELEASE:
        if (released) {
            pc->phase = VCU_PEDAL_CHECK_FLOOR_IT;
        }
        break;
    case VCU_PEDAL_CHECK_FLOOR_IT:
        if (floored) {
            pc->phase = VCU_PEDAL_CHECK_RELEASE_AGAIN;
        }
        break;
    case VCU_PEDAL_CHECK_RELEASE_AGAIN:
        if (released) {
            pc->phase = VCU_PEDAL_CHECK_DONE;
        }
        break;
    default:
        break;
    }

    if (!brake_engaged && brakes_ok(sn)) {
        pc->brake_released = true;
    } else if (brake_engaged && pc->brake_released) {
        pc->brake_engaged = true;
    }

    return (pc->phase == VCU_PEDAL_CHECK_DONE && pc->brake_engaged) ? VCU_PEDAL_CHECK_PASS
                                                                    : VCU_PEDAL_CHECK_NONE;
}
