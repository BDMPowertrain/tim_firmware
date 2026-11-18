/* The Power limiter (issue 60, ADR 0017). See vcu_power_limiter.h. */
#include "vcu_power_limiter.h"

/* 2 pi x 10^6: rad/s from rpm is rpm x 2 pi / 60. */
#define TWO_PI_MICRO 6283185u

void vcu_power_limiter_init(vcu_power_limiter_t *pl)
{
    pl->trim_ppm = VCU_TRIM_MIN_PPM;
    pl->frames_seen = 0u;
    pl->binding = false;
    pl->binding_ms = 0u;
}

/* Power x efficiency / speed, in 0.1 N.m, rounded down:
   T [0.1 N.m] = 10 x P x (e / 1000) x 60 / (2 pi x rpm)
               = P x e x 600 x 10^6 / (1000 x 2 pi x 10^6 x rpm). */
static int16_t torque_ceiling(uint32_t power_w, int16_t speed_rpm)
{
    uint32_t rpm = (uint32_t)(speed_rpm < 0 ? -(int32_t)speed_rpm : (int32_t)speed_rpm);
    if (rpm < VCU_SPEED_FLOOR_RPM) {
        rpm = VCU_SPEED_FLOOR_RPM;
    }
    const uint64_t num = (uint64_t)power_w * VCU_EFFICIENCY_PERMILLE * 600u * 1000u;
    const uint64_t den = (uint64_t)TWO_PI_MICRO * rpm;
    const uint64_t t = num / den;
    return (t > (uint64_t)INT16_MAX) ? INT16_MAX : (int16_t)t;
}

/* The ceiling with the trim applied, rounded down. */
static uint32_t trimmed(uint32_t ceiling_w, int32_t trim_ppm)
{
    return (uint32_t)(((uint64_t)ceiling_w * (uint64_t)(1000000 + trim_ppm)) / 1000000u);
}

/* One integral step on one frame: a quarter of the error, as a fraction of
   the ceiling, then the bounds. */
static int32_t trim_step(int32_t trim_ppm, uint32_t ceiling_w, const vcu_limiter_in_t *in)
{
    if (ceiling_w == 0u) {
        return trim_ppm; /* nothing to be a fraction of */
    }
    /* 0.1 A x 0.1 V is 0.01 W. */
    const int64_t pack_w = ((int64_t)in->pack_current_da * (int64_t)in->pack_voltage_dv) / 100;
    int64_t t = trim_ppm + (((int64_t)ceiling_w - pack_w) * VCU_TRIM_GAIN_PPM) / (int64_t)ceiling_w;
    if (t > VCU_TRIM_MAX_PPM) {
        t = VCU_TRIM_MAX_PPM;
    }
    if (t < VCU_TRIM_MIN_PPM) {
        t = VCU_TRIM_MIN_PPM;
    }
    return (int32_t)t;
}

void vcu_power_limiter_step(vcu_power_limiter_t *pl, const vcu_limiter_in_t *in,
                            vcu_limiter_out_t *out)
{
    const bool new_frame = in->power_frames != pl->frames_seen;
    pl->frames_seen = in->power_frames;
    if (!in->power_valid) {
        pl->trim_ppm = VCU_TRIM_MIN_PPM; /* and from there when it is back */
    }

    /* 0.1 A x 0.1 V is 0.01 W; 65535 x 65535 fits in 32 bits. */
    const uint32_t discharge_w =
        ((uint32_t)in->discharge_limit_da * (uint32_t)in->pack_voltage_dv) / 100u;
    const bool discharge_lower = discharge_w < VCU_POWER_TARGET_W;
    const uint32_t ceiling_w = discharge_lower ? discharge_w : VCU_POWER_TARGET_W;

    /* The trim moves on a new frame while the limiter binds on the trim it
       has, and has bound long enough for the frame to have seen it; the
       step then sends with the new trim. */
    const bool binds =
        in->request > torque_ceiling(trimmed(ceiling_w, pl->trim_ppm), in->motor_speed_rpm);
    if (binds && !pl->binding) {
        pl->binding_ms = in->now_ms;
    }
    pl->binding = binds;
    const bool settled = binds && (uint32_t)(in->now_ms - pl->binding_ms) >= VCU_TRIM_SETTLE_MS;
    if (in->power_valid && new_frame && settled) {
        pl->trim_ppm = trim_step(pl->trim_ppm, ceiling_w, in);
    }

    out->power_ceiling_w = ceiling_w;
    out->trim_ppm = pl->trim_ppm;
    out->lower = discharge_lower ? VCU_CEILING_DISCHARGE : VCU_CEILING_TARGET;
    out->torque_ceiling = torque_ceiling(trimmed(ceiling_w, pl->trim_ppm), in->motor_speed_rpm);
    out->torque = (in->request < out->torque_ceiling) ? in->request : out->torque_ceiling;
    out->active = out->torque < in->request;
}
