/*
 * The Power limiter (issue 60, ADR 0017): the limiter slot of the VCU's
 * torque path. Every control step it turns the Power ceiling into a torque
 * ceiling and sends the lower of that and the driver's request, so it only
 * ever lowers torque (EV.3.1.3). It is not a Torque cut: the driver keeps
 * whatever fits under the ceiling.
 *
 *   Power ceiling   the lower of VCU_POWER_TARGET_W and the Discharge limit
 *                   x pack voltage, both from BMS_Power (EV.3.3.1).
 *   torque ceiling  Power ceiling x (1 + trim) x VCU_EFFICIENCY_PERMILLE /
 *                   motor speed, the speed floored at VCU_SPEED_FLOOR_RPM
 *                   and taken as its size (backwards counts too). Rounded
 *                   down.
 *   trim            integral only, between VCU_TRIM_MIN_PPM and 0: it may
 *                   lower the ceiling, never raise it. It moves once per new
 *                   BMS_Power frame and only while the limiter binds, and
 *                   has bound for VCU_TRIM_SETTLE_MS without a break, by
 *                   VCU_TRIM_GAIN_PPM x (Power ceiling - pack power) / Power
 *                   ceiling, pack power being that frame's pack current x
 *                   pack voltage (the same sweep). It is kept while the
 *                   limiter does not bind, so it is kept between pulls. It
 *                   is VCU_TRIM_MIN_PPM from reset and for as long as
 *                   BMS_Power is not VALID, so it starts there after
 *                   BMS_Power has been invalid.
 *
 * The limiter binds, and is active, when the torque it lets through is
 * below the request. lower says which of the two ceilings is the lower,
 * whether or not the limiter binds, for the Data log.
 *
 * Its caller hands it the request after the gates (zero outside Ready to
 * Drive and in a Torque cut), so a car that is not being driven never
 * binds and never moves the trim. Nothing here reads the Energy Meter
 * (ADR 0017), and nothing here touches the inverter's torque-limit bytes:
 * the limiter's only output is the torque.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock, no float.
 */
#ifndef VCU_POWER_LIMITER_H
#define VCU_POWER_LIMITER_H

#include <stdbool.h>
#include <stdint.h>

/* 80 kW (EV.3.3.1) less a 10% margin held by the written error budget:
   sensor accuracy, sample delay, trim lag (ADR 0017). */
#define VCU_POWER_TARGET_W      72000u
/* Motor and inverter efficiency, deliberately low (ADR 0017). */
#define VCU_EFFICIENCY_PERMILLE 850u
/* Below this the speed is taken as this: no division by 0 at standstill. */
#define VCU_SPEED_FLOOR_RPM     100u
/* The trim's bounds, parts per million of the Power ceiling. */
#define VCU_TRIM_MIN_PPM        (-150000)
#define VCU_TRIM_MAX_PPM        0
/* The trim's step per BMS_Power frame for the whole Power ceiling of
   error, in ppm: 0.25 of the error, measured in fractions of the ceiling. */
#define VCU_TRIM_GAIN_PPM       250000
/* A frame moves the trim only once the limiter has bound this long without
   a break: one BMS sweep (20 ms), the frame's way to the step, one step
   (10 ms) and the inverter's torque rise. An earlier frame may have been
   sampled before the torque rose, reads the pedal-up power and would wind
   the trim up for nothing at the start of every pull. */
#define VCU_TRIM_SETTLE_MS      50u

/* Which ceiling is the lower (out.lower). On a tie, the target. */
typedef enum {
    VCU_CEILING_TARGET    = 0, /* VCU_POWER_TARGET_W */
    VCU_CEILING_DISCHARGE = 1, /* Discharge limit x pack voltage */
} vcu_ceiling_t;

typedef struct {
    uint32_t now_ms;
    int16_t  request;           /* the driver's after the gates, 0.1 N.m, never negative */
    int16_t  motor_speed_rpm;    /* INV_MotorPosition */
    bool     power_valid;        /* the BMS_Power mailbox is VALID */
    uint32_t power_frames;       /* its good frames so far: a change is a new frame */
    int16_t  pack_current_da;    /* BMS_Power, 0.1 A, discharging positive */
    uint16_t pack_voltage_dv;    /* BMS_Power, 0.1 V */
    uint16_t discharge_limit_da; /* BMS_Power, 0.1 A */
} vcu_limiter_in_t;

typedef struct {
    int16_t  torque;          /* sent: the lower of the request and torque_ceiling */
    bool     active;          /* torque is below the request */
    uint8_t  lower;           /* vcu_ceiling_t */
    uint32_t power_ceiling_w; /* the Power ceiling, before the trim */
    int16_t  torque_ceiling;  /* 0.1 N.m, after the trim */
    int32_t  trim_ppm;        /* the trim used this step */
} vcu_limiter_out_t;

/* The limiter's own state. vcu_power_limiter_init() it; touch nothing inside. */
typedef struct {
    int32_t  trim_ppm;
    uint32_t frames_seen; /* power_frames at the last step */
    bool     binding;     /* the last step bound */
    uint32_t binding_ms;  /* since when, without a break */
} vcu_power_limiter_t;

void vcu_power_limiter_init(vcu_power_limiter_t *pl);

void vcu_power_limiter_step(vcu_power_limiter_t *pl, const vcu_limiter_in_t *in,
                            vcu_limiter_out_t *out);

#endif /* VCU_POWER_LIMITER_H */
