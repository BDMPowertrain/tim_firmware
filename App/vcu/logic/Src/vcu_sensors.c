/* vcu_sensors.c — see vcu_sensors.h. */

#include "vcu_sensors.h"

#include <stddef.h>

#include "adc_window.h"

/* Issue 20: APPS1 rises with the pedal, APPS2 falls. */
static const bool appsFalls[VCU_APPS_COUNT] = { false, true };

static vcu_apps_reading_t apps_reading(uint16_t raw, int32_t min, int32_t max, bool falls)
{
    vcu_apps_reading_t r = { 0u, VCU_SENSOR_OK };
    const int32_t span = max - min;
    const int32_t margin = span * VCU_APPS_MARGIN_PERCENT / 100;
    const int32_t v = (int32_t)raw;

    if (raw == ADC_WINDOW_EMPTY) {
        r.status = VCU_SENSOR_NO_SAMPLE;
        return r;
    }
    if (span <= 0) {
        r.status = VCU_SENSOR_NOT_CALIBRATED;
        return r;
    }
    if (v < min - margin || v > max + margin) {
        r.status = VCU_SENSOR_OUT_OF_RANGE;
        return r;
    }

    const int32_t fromZero = falls ? max - v : v - min;
    int32_t t = fromZero * (int32_t)VCU_TRAVEL_FULL / span;

    if (t < 0) {
        t = 0;
    } else if (t > (int32_t)VCU_TRAVEL_FULL) {
        t = (int32_t)VCU_TRAVEL_FULL;
    }
    r.travel = (uint16_t)t;
    return r;
}

static bool in_brake_window(int32_t counts)
{
    return counts >= (int32_t)VCU_BRAKE_MIN_COUNTS && counts <= (int32_t)VCU_BRAKE_MAX_COUNTS;
}

static vcu_brake_reading_t brake_reading(uint16_t raw, int32_t zero)
{
    vcu_brake_reading_t r = { 0u, VCU_SENSOR_OK };

    if (raw == ADC_WINDOW_EMPTY) {
        r.status = VCU_SENSOR_NO_SAMPLE;
    } else if (!in_brake_window(zero)) {
        r.status = VCU_SENSOR_NOT_CALIBRATED;
    } else if (!in_brake_window(raw)) {
        r.status = VCU_SENSOR_OUT_OF_RANGE;
    } else {
        r.pressure = (raw > zero) ? (uint16_t)((int32_t)raw - zero) : 0u;
    }
    return r;
}

void vcu_sensors_compute(const vcu_sensor_raw_t *raw, const vcu_sensor_cal_t *cal,
                         vcu_sensors_t *out)
{
    /* Issue 20: supply i feeds APPS i and brake sensor i. No sample on
       the supply fails it too (ADC_WINDOW_EMPTY is above the window). */
    for (size_t i = 0u; i < 2u; i++) {
        out->supply_ok[i] = raw->supply[i] >= VCU_SUPPLY_MIN_COUNTS
                            && raw->supply[i] <= VCU_SUPPLY_MAX_COUNTS;
    }
    for (size_t i = 0u; i < VCU_APPS_COUNT; i++) {
        out->apps[i] = apps_reading(raw->apps[i], cal->apps_min[i], cal->apps_max[i],
                                    appsFalls[i]);
        if (!out->supply_ok[i] && out->apps[i].status != VCU_SENSOR_NO_SAMPLE) {
            out->apps[i] = (vcu_apps_reading_t){ 0u, VCU_SENSOR_SUPPLY_FAULT };
        }
    }
    for (size_t i = 0u; i < VCU_BRAKE_COUNT; i++) {
        out->brake[i] = brake_reading(raw->brake[i], cal->brake_zero[i]);
        if (!out->supply_ok[i] && out->brake[i].status != VCU_SENSOR_NO_SAMPLE) {
            out->brake[i] = (vcu_brake_reading_t){ 0u, VCU_SENSOR_SUPPLY_FAULT };
        }
    }
}

const char *vcu_sensor_status_name(vcu_sensor_status_t status)
{
    switch (status) {
    case VCU_SENSOR_OK:             return "ok";
    case VCU_SENSOR_NO_SAMPLE:      return "no sample";
    case VCU_SENSOR_SUPPLY_FAULT:   return "supply fault";
    case VCU_SENSOR_NOT_CALIBRATED: return "not calibrated";
    case VCU_SENSOR_OUT_OF_RANGE:   return "out of range";
    default:                        return "?";
    }
}
