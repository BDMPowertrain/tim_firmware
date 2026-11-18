/* vcu_calibration.c — see vcu_calibration.h. */

#include "vcu_calibration.h"

#include <stddef.h>
#include <string.h>

#include "adc_window.h"
#include "crc32.h"
#include "vcu_params.h"

static vcu_cal_result_t result(vcu_cal_reason_t reason, vcu_cal_sensor_t sensor)
{
    return (vcu_cal_result_t){ reason, sensor };
}

static bool inside(int32_t counts, int32_t low, int32_t high)
{
    return counts >= low && counts <= high;
}

vcu_cal_result_t vcu_cal_check(const vcu_cal_values_t *v, bool present)
{
    static const vcu_cal_sensor_t apps[VCU_APPS_COUNT] = { VCU_CAL_SENSOR_APPS1,
                                                           VCU_CAL_SENSOR_APPS2 };
    static const vcu_cal_sensor_t brake[VCU_BRAKE_COUNT] = { VCU_CAL_SENSOR_BRAKE_FRONT,
                                                             VCU_CAL_SENSOR_BRAKE_REAR };

    if (!present) {
        return result(VCU_CAL_MISSING, VCU_CAL_SENSOR_NONE);
    }
    if (v->revision != VCU_CALIBRATION_REVISION) {
        return result(VCU_CAL_REVISION, VCU_CAL_SENSOR_NONE);
    }
    for (size_t i = 0; i < VCU_APPS_COUNT; i++) {
        const int32_t min = v->apps_min[i];
        const int32_t max = v->apps_max[i];

        if (min >= max) {
            return result(VCU_CAL_APPS_ORDER, apps[i]);
        }
        if (max - min < (int32_t)VCU_APPS_MIN_SPAN) {
            return result(VCU_CAL_APPS_SPAN, apps[i]);
        }
        if (!inside(min, VCU_APPS_MIN_COUNTS, VCU_APPS_MAX_COUNTS) ||
            !inside(max, VCU_APPS_MIN_COUNTS, VCU_APPS_MAX_COUNTS)) {
            return result(VCU_CAL_APPS_WINDOW, apps[i]);
        }
    }
    for (size_t i = 0; i < VCU_BRAKE_COUNT; i++) {
        const int32_t zero = v->brake_zero[i];

        if (!inside(zero, VCU_BRAKE_MIN_COUNTS, VCU_BRAKE_MAX_COUNTS) ||
            !inside(zero + v->brake_engaged, VCU_BRAKE_MIN_COUNTS, VCU_BRAKE_MAX_COUNTS)) {
            return result(VCU_CAL_BRAKE_WINDOW, brake[i]);
        }
    }
    if (v->brake_engaged < VCU_BRAKE_ENGAGED_MIN) {
        return result(VCU_CAL_BRAKE_THRESHOLD, VCU_CAL_SENSOR_NONE);
    }
    return result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE);
}

static uint32_t crc_value(uint32_t crc, int32_t value)
{
    const uint32_t u = (uint32_t)value;
    const uint8_t bytes[4] = { (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16),
                               (uint8_t)(u >> 24) };

    return crc32_update(crc, bytes, sizeof(bytes));
}

/* One value at a time: this runs at start-up on initTask's 512-byte stack
   (vcu_step_init()), which has no room for the whole set as bytes. */
uint32_t vcu_cal_fingerprint(const vcu_cal_values_t *v)
{
    uint32_t crc = 0u;

    for (size_t i = 0; i < VCU_APPS_COUNT; i++) {
        crc = crc_value(crc, v->apps_min[i]);
    }
    for (size_t i = 0; i < VCU_APPS_COUNT; i++) {
        crc = crc_value(crc, v->apps_max[i]);
    }
    for (size_t i = 0; i < VCU_BRAKE_COUNT; i++) {
        crc = crc_value(crc, v->brake_zero[i]);
    }
    crc = crc_value(crc, v->brake_engaged);
    return crc_value(crc, v->revision);
}

void vcu_cal_capture_init(vcu_cal_capture_t *c)
{
    memset(c, 0, sizeof(*c));
    for (size_t i = 0; i < VCU_CAL_CHANNELS; i++) {
        c->low[i] = UINT16_MAX;
    }
}

void vcu_cal_capture_add(vcu_cal_capture_t *c, const vcu_sensor_raw_t *raw)
{
    const uint16_t counts[VCU_CAL_CHANNELS] = { raw->apps[0], raw->apps[1], raw->brake[0],
                                                raw->brake[1] };

    for (size_t i = 0; i < VCU_CAL_CHANNELS; i++) {
        if (counts[i] == ADC_WINDOW_EMPTY) {
            c->empty = true;
            continue;
        }
        c->sum[i] += counts[i];
        if (counts[i] < c->low[i]) {
            c->low[i] = counts[i];
        }
        if (counts[i] > c->high[i]) {
            c->high[i] = counts[i];
        }
    }
    c->count++;
}

/* The capture's channels are in vcu_cal_sensor_t order from APPS1. */
static vcu_cal_sensor_t channel_sensor(size_t channel)
{
    return (vcu_cal_sensor_t)(VCU_CAL_SENSOR_APPS1 + channel);
}

static vcu_cal_result_t held(const vcu_cal_capture_t *c)
{
    if (c->count == 0u || c->empty) {
        return result(VCU_CAL_NO_SAMPLE, VCU_CAL_SENSOR_NONE);
    }
    for (size_t i = 0; i < VCU_CAL_CHANNELS; i++) {
        if ((uint32_t)(c->high[i] - c->low[i]) > VCU_CAL_STEADY_COUNTS) {
            return result(VCU_CAL_NOT_STEADY, channel_sensor(i));
        }
    }
    return result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE);
}

static int32_t average(const vcu_cal_capture_t *c, size_t channel)
{
    return (int32_t)((c->sum[channel] + c->count / 2u) / c->count);
}

vcu_cal_result_t vcu_cal_from_captures(const vcu_cal_capture_t *released,
                                       const vcu_cal_capture_t *floored,
                                       const vcu_cal_capture_t *braked, vcu_cal_values_t *out)
{
    const vcu_cal_capture_t *const all[] = { released, floored, braked };

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const vcu_cal_result_t r = held(all[i]);
        if (r.reason != VCU_CAL_OK) {
            return r;
        }
    }

    /* Both APPS must move, each its own way (vcu_sensors.c: APPS1 rises,
       APPS2 falls). */
    const int32_t apps1Rest = average(released, 0u);
    const int32_t apps1Floor = average(floored, 0u);
    const int32_t apps2Rest = average(released, 1u);
    const int32_t apps2Floor = average(floored, 1u);

    if (apps1Floor <= apps1Rest) {
        return result(VCU_CAL_APPS_DIRECTION, VCU_CAL_SENSOR_APPS1);
    }
    if (apps2Floor >= apps2Rest) {
        return result(VCU_CAL_APPS_DIRECTION, VCU_CAL_SENSOR_APPS2);
    }

    int32_t rise = INT32_MAX;
    for (size_t i = 0; i < VCU_BRAKE_COUNT; i++) {
        const int32_t r = average(braked, 2u + i) - average(released, 2u + i);
        if (r < rise) {
            rise = r;
        }
    }

    *out = (vcu_cal_values_t){
        .apps_min = { apps1Rest, apps2Floor },
        .apps_max = { apps1Floor, apps2Rest },
        .brake_zero = { average(released, 2u), average(released, 3u) },
        .brake_engaged = (rise > 0) ? rise * VCU_BRAKE_ENGAGED_PERCENT / 100 : 0,
        .revision = VCU_CALIBRATION_REVISION,
    };
    return vcu_cal_check(out, true);
}

const char *vcu_cal_reason_name(vcu_cal_reason_t reason)
{
    switch (reason) {
    case VCU_CAL_OK:              return "calibrated";
    case VCU_CAL_MISSING:         return "not stored";
    case VCU_CAL_REVISION:        return "captured for another sensor path (revision)";
    case VCU_CAL_APPS_ORDER:      return "minimum not below maximum";
    case VCU_CAL_APPS_SPAN:       return "span too small";
    case VCU_CAL_APPS_WINDOW:     return "end point outside the valid window";
    case VCU_CAL_BRAKE_WINDOW:    return "zero or threshold outside the valid window";
    case VCU_CAL_BRAKE_THRESHOLD: return "brake rise too small (press the brake harder)";
    case VCU_CAL_NO_SAMPLE:       return "a sample is missing";
    case VCU_CAL_NOT_STEADY:      return "moved while it was held";
    case VCU_CAL_APPS_DIRECTION:  return "did not move with the pedal";
    default:                      return "?";
    }
}

const char *vcu_cal_sensor_name(vcu_cal_sensor_t sensor)
{
    switch (sensor) {
    case VCU_CAL_SENSOR_NONE:        return "";
    case VCU_CAL_SENSOR_APPS1:       return "APPS1";
    case VCU_CAL_SENSOR_APPS2:       return "APPS2";
    case VCU_CAL_SENSOR_BRAKE_FRONT: return "front brake";
    case VCU_CAL_SENSOR_BRAKE_REAR:  return "rear brake";
    default:                         return "?";
    }
}
