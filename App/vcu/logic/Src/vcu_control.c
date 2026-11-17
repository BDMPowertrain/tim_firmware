#include "vcu_control.h"

#include <string.h>

#include "adc_window.h"
#include "event_codes.h"
#include "vcu_tx.h"

const control_rx_t vcu_rx_frames[VCU_RX_COUNT] = {
    [VCU_RX_BMS_STATUS]            = CONTROL_RX_BMS_STATUS,
    [VCU_RX_BMS_LIMITS]            = CONTROL_RX_BMS_LIMITS,
    [VCU_RX_BMS_POWER]             = CONTROL_RX_BMS_POWER,
    [VCU_RX_INV_MOTOR_POSITION]    = CONTROL_RX_INV_MOTOR_POSITION,
    [VCU_RX_INV_VOLTAGE]           = CONTROL_RX_INV_VOLTAGE,
    [VCU_RX_INV_INTERNAL_STATES]   = CONTROL_RX_INV_INTERNAL_STATES,
    [VCU_RX_INV_FAULT_CODES]       = CONTROL_RX_INV_FAULT_CODES,
    [VCU_RX_INV_TORQUE_CAPABILITY] = CONTROL_RX_INV_TORQUE_CAPABILITY,
    [VCU_RX_EM_MEASUREMENT]        = CONTROL_RX_EM_MEASUREMENT,
    [VCU_RX_EM_STATUS]             = CONTROL_RX_EM_STATUS,
    [VCU_RX_IMD_INFO_GENERAL]      = CONTROL_RX_IMD_INFO_GENERAL,
};

void vcu_control_init(vcu_control_t *vc, const vcu_control_config_t *config)
{
    memset(vc, 0, sizeof(*vc));
    vc->config = *config;
    vc->state = VCU_STATE_STARTUP;
}

/* ------------------------------------------------------------------ */
/* The snapshot                                                       */
/* ------------------------------------------------------------------ */

/* What the step read from the mailboxes. A fault or a voltage is taken
   from a VALID mailbox only where noted: a Substitute never reports a
   fault. */
typedef struct {
    bool     bms_status_valid;
    bool     bms_limits_valid;
    bool     bms_power_valid;
    bool     bms_valid;      /* all three BMS frames */
    bool     inverter_valid; /* every inverter frame the VCU acts on */
    bool     em_valid;
    bool     bms_fault;      /* a VALID BMS_Status reports a fault */
    bool     inverter_fault; /* a VALID INV_FaultCodes reports a fault */
    int16_t  bus_dv;         /* INV_Voltage, or its Substitute (0 V) */
    uint16_t pack_dv;        /* BMS_Limits, or its Substitute (0 V) */
    bool     voltages_valid; /* both of those are VALID */
} bus_view_t;

static bool valid(const vcu_control_in_t *in, vcu_rx_t row)
{
    return in->mailboxes[row].state == MAILBOX_VALID;
}

static const uint8_t *frame(const vcu_control_in_t *in, vcu_rx_t row)
{
    return mailbox_read(&in->mailboxes[row], &control_rx_config[vcu_rx_frames[row]]);
}

static bool bms_reports_fault(const struct can_control_bms_status_t *s)
{
    const uint8_t active = s->over_voltage_active | s->under_voltage_active |
                           s->over_temp_active | s->under_temp_active |
                           s->measurement_active | s->open_wire_active |
                           s->cell_monitor_active | s->internal_active |
                           s->charger_disobeys_active | s->overcurrent_active;

    return s->bms_ok == 0u || active != 0u ||
           s->state == CAN_CONTROL_BMS_STATUS_STATE_FAULT_CHOICE ||
           s->state == CAN_CONTROL_BMS_STATUS_STATE_FAULT_HELD_CHOICE;
}

static void read_bus(const vcu_control_in_t *in, bus_view_t *bus)
{
    struct can_control_bms_status_t bmsStatus;
    struct can_control_bms_limits_t bmsLimits;
    struct can_control_inv_voltage_t invVoltage;
    struct can_control_inv_fault_codes_t invFaults;

    (void)can_control_bms_status_unpack(&bmsStatus, frame(in, VCU_RX_BMS_STATUS),
                                        CAN_CONTROL_BMS_STATUS_LENGTH);
    (void)can_control_bms_limits_unpack(&bmsLimits, frame(in, VCU_RX_BMS_LIMITS),
                                        CAN_CONTROL_BMS_LIMITS_LENGTH);
    (void)can_control_inv_voltage_unpack(&invVoltage, frame(in, VCU_RX_INV_VOLTAGE),
                                         CAN_CONTROL_INV_VOLTAGE_LENGTH);
    (void)can_control_inv_fault_codes_unpack(&invFaults, frame(in, VCU_RX_INV_FAULT_CODES),
                                             CAN_CONTROL_INV_FAULT_CODES_LENGTH);

    bus->bms_status_valid = valid(in, VCU_RX_BMS_STATUS);
    bus->bms_limits_valid = valid(in, VCU_RX_BMS_LIMITS);
    bus->bms_power_valid = valid(in, VCU_RX_BMS_POWER);
    bus->bms_valid = bus->bms_status_valid && bus->bms_limits_valid && bus->bms_power_valid;
    /* INV_TorqueCapability is logged only (can/control.dbc). */
    bus->inverter_valid = valid(in, VCU_RX_INV_MOTOR_POSITION) &&
                          valid(in, VCU_RX_INV_VOLTAGE) &&
                          valid(in, VCU_RX_INV_INTERNAL_STATES) &&
                          valid(in, VCU_RX_INV_FAULT_CODES);
    bus->em_valid = valid(in, VCU_RX_EM_MEASUREMENT);
    bus->bms_fault = bus->bms_status_valid && bms_reports_fault(&bmsStatus);
    bus->inverter_fault = valid(in, VCU_RX_INV_FAULT_CODES) &&
                          (invFaults.post_faults != 0u || invFaults.run_faults != 0u);
    bus->bus_dv = invVoltage.dc_bus_voltage;
    bus->pack_dv = bmsLimits.pack_voltage;
    bus->voltages_valid = valid(in, VCU_RX_INV_VOLTAGE) && bus->bms_limits_valid;
}

/* The bus is at least percent of the pack voltage. */
static bool bus_at_least(const bus_view_t *bus, uint32_t percent)
{
    if (bus->bus_dv < 0) {
        return false;
    }
    return (uint32_t)bus->bus_dv * 100u >= (uint32_t)bus->pack_dv * percent;
}

/* The Tractive System is up: at least 60 V on the bus, so a pack voltage
   of 0 cannot pass, and at least 90% of the pack. */
static bool ts_up(const bus_view_t *bus)
{
    return bus->bus_dv >= VCU_TS_MIN_DV && bus_at_least(bus, VCU_TS_UP_PERCENT);
}

static bool all_sampled(const vcu_sensor_raw_t *raw)
{
    const uint16_t all[] = { raw->apps[0],  raw->apps[1],   raw->brake[0],
                             raw->brake[1], raw->supply[0], raw->supply[1] };

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i] == ADC_WINDOW_EMPTY) {
            return false;
        }
    }
    return true;
}

/* Brake engaged: either sensor OK and above the threshold, each with its
   own hysteresis. A sensor that is not OK reads 0 and so never engages. */
static bool brake_engaged(vcu_control_t *vc, const vcu_sensors_t *sensors)
{
    const uint16_t on = vc->config.brake_engaged;
    const uint16_t off = (on > VCU_BRAKE_HYSTERESIS) ? (uint16_t)(on - VCU_BRAKE_HYSTERESIS) : 0u;
    bool any = false;

    for (size_t i = 0; i < VCU_BRAKE_COUNT; i++) {
        const vcu_brake_reading_t *r = &sensors->brake[i];

        if (r->status != VCU_SENSOR_OK) {
            vc->sensor_engaged[i] = false;
        } else if (vc->sensor_engaged[i]) {
            vc->sensor_engaged[i] = r->pressure > off;
        } else {
            vc->sensor_engaged[i] = r->pressure > on;
        }
        any = any || vc->sensor_engaged[i];
    }
    return any;
}

/* A press of the start button: the first step it reads pressed after
   reading released for VCU_START_RELEASE_MS. A button never seen released
   since reset has no press. */
static bool start_press(vcu_control_t *vc, const vcu_control_in_t *in)
{
    if (!in->start_pressed) {
        if (!vc->button_released) {
            vc->button_released = true;
            vc->button_released_ms = in->now_ms;
        }
        return false;
    }
    const bool press = vc->button_released &&
                       (uint32_t)(in->now_ms - vc->button_released_ms) >= VCU_START_RELEASE_MS;
    vc->button_released = false;
    return press;
}

static uint16_t higher_travel(const vcu_sensors_t *sensors)
{
    const uint16_t a = sensors->apps[0].travel;
    const uint16_t b = sensors->apps[1].travel;
    return (a > b) ? a : b;
}

static bool cut_active(const vcu_control_out_t *out, vcu_cut_t cause)
{
    return (out->cuts & (1u << cause)) != 0u;
}

static bool brake_sensor_failed(const vcu_sensors_t *sn)
{
    return sn->brake[0].status != VCU_SENSOR_OK || sn->brake[1].status != VCU_SENSOR_OK;
}

static uint16_t lower_travel(const vcu_sensors_t *sensors)
{
    const uint16_t a = sensors->apps[0].travel;
    const uint16_t b = sensors->apps[1].travel;
    return (a < b) ? a : b;
}

/* The driver's request from the lower Pedal travel, 0.1 N.m: 0 below 5%,
   then linear from 0 at 5% to max_torque at 100%, so there is no step at
   5%. Never negative, and never above max_torque. */
static int16_t driver_request(const vcu_control_t *vc, uint16_t travel)
{
    if (travel < VCU_TRAVEL_RELEASED) {
        return 0;
    }
    if (travel > VCU_TRAVEL_FULL) {
        travel = VCU_TRAVEL_FULL;
    }
    const uint32_t max = (vc->config.max_torque > (uint16_t)INT16_MAX) ? (uint32_t)INT16_MAX
                                                                        : vc->config.max_torque;
    return (int16_t)((max * (uint32_t)(travel - VCU_TRAVEL_RELEASED)) /
                     (VCU_TRAVEL_FULL - VCU_TRAVEL_RELEASED));
}

/* Everything that refuses the Start procedure now. */
static uint16_t start_blockers(const vcu_control_t *vc, const vcu_control_in_t *in,
                               const bus_view_t *bus, const vcu_control_out_t *out)
{
    const vcu_sensors_t *sn = &out->sensors;
    uint16_t b = 0u;

    if (vc->state != VCU_STATE_TS_ACTIVE) {
        b |= VCU_START_NOT_TS_ACTIVE;
    }
    if (!out->brake_engaged) {
        b |= VCU_START_BRAKE;
    }
    if (higher_travel(sn) >= VCU_TRAVEL_RELEASED) {
        b |= VCU_START_PEDAL;
    }
    if (sn->apps[0].status != VCU_SENSOR_OK || sn->apps[1].status != VCU_SENSOR_OK) {
        b |= VCU_START_PEDAL_SENSOR;
    }
    if (brake_sensor_failed(sn)) {
        b |= VCU_START_BRAKE_SENSOR;
    }
    if (vc->config.params_defaulted) {
        b |= VCU_START_PARAMS_DEFAULTED;
    }
    if (!bus->bms_valid || !bus->inverter_valid) {
        b |= VCU_START_MAILBOX;
    }
    if (bus->bms_fault) {
        b |= VCU_START_BMS_FAULT;
    }
    if (bus->inverter_fault) {
        b |= VCU_START_INVERTER_FAULT;
    }
    if (!in->sdc_closed) {
        b |= VCU_START_SDC_OPEN;
    }
    if (in->bus_off_latched) {
        b |= VCU_START_BUS_OFF;
    }
    if (!ts_up(bus)) {
        b |= VCU_START_BUS_VOLTAGE;
    }
    if (cut_active(out, VCU_CUT_TIMING)) {
        b |= VCU_START_TIMING_FAULT;
    }
    if (out->cuts != 0u) {
        b |= VCU_START_TORQUE_CUT;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* Transitions                                                        */
/* ------------------------------------------------------------------ */

static void post_event(vcu_control_out_t *out, uint16_t code,
                       const uint8_t data[EVENT_DATA_SIZE])
{
    if (out->event_count < VCU_CONTROL_EVENTS_MAX) {
        out->events[out->event_count].code = code;
        memcpy(out->events[out->event_count].data, data, EVENT_DATA_SIZE);
        out->event_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Torque cuts (ADR 0006)                                             */
/* ------------------------------------------------------------------ */

static void post_cut_event(const vcu_control_t *vc, vcu_control_out_t *out, vcu_cut_t cause,
                           bool started)
{
    const vcu_sensors_t *sn = &out->sensors;
    const event_torque_cut_t ev = {
        .cause = (uint8_t)cause,
        .started = started,
        .state = vc->state,
        .travel = { sn->apps[0].travel, sn->apps[1].travel },
        .apps_status = { (uint8_t)sn->apps[0].status, (uint8_t)sn->apps[1].status },
        .brake_status = { (uint8_t)sn->brake[0].status, (uint8_t)sn->brake[1].status },
    };
    uint8_t data[EVENT_DATA_SIZE];

    event_data_torque_cut(data, &ev);
    post_event(out, EVENT_TORQUE_CUT, data);
}

/* One Torque cut: it starts once its cause has held for persist_ms, and
   ends only once the cause is gone and the higher Pedal travel is below
   5%, so torque never returns under a pressed pedal (ADR 0006). Each start
   and end is an Event. */
static void run_cut(vcu_control_t *vc, const vcu_control_in_t *in, vcu_control_out_t *out,
                    vcu_cut_t cause, bool holds, uint32_t persist_ms)
{
    vcu_cut_timer_t *c = &vc->cut[cause];

    if (!holds) {
        c->pending = false;
    } else if (!c->pending) {
        c->pending = true;
        c->since_ms = in->now_ms;
    }

    if (!c->active) {
        if (holds && (uint32_t)(in->now_ms - c->since_ms) >= persist_ms) {
            c->active = true;
            post_cut_event(vc, out, cause, true);
        }
    } else if (!holds && higher_travel(&out->sensors) < VCU_TRAVEL_RELEASED) {
        c->active = false;
        post_cut_event(vc, out, cause, false);
    }

    if (c->active) {
        out->cuts |= (uint16_t)(1u << cause);
    }
}

/* T.4.2.4: implausible is more than 10 points apart, or either sensor not
   OK (out of range, its supply out of its window, no sample). */
static bool apps_implausible(const vcu_sensors_t *sn)
{
    const uint16_t a = sn->apps[0].travel;
    const uint16_t b = sn->apps[1].travel;
    const uint16_t diff = (a > b) ? (uint16_t)(a - b) : (uint16_t)(b - a);

    return sn->apps[0].status != VCU_SENSOR_OK || sn->apps[1].status != VCU_SENSOR_OK ||
           diff > VCU_APPS_AGREE;
}

static void enter(vcu_control_t *vc, const vcu_control_in_t *in, vcu_control_out_t *out,
                  const bus_view_t *bus, uint8_t to, vcu_cause_t cause, uint16_t causes)
{
    const event_vcu_state_t ev = {
        .from = vc->state,
        .to = to,
        .cause = (uint8_t)cause,
        .bus_dv = bus->bus_dv,
        .pack_dv = bus->pack_dv,
        .causes = causes,
    };
    uint8_t data[EVENT_DATA_SIZE];

    event_data_vcu_state(data, &ev);
    post_event(out, EVENT_VCU_STATE, data);
    /* Leaving Ready to Drive needs the Start procedure again: the fault
       LED is solid until it is done, or until the Tractive System is
       down and there is nothing to restart (ADR 0006). */
    if (to == VCU_STATE_READY_TO_DRIVE || to == VCU_STATE_GLV_ON) {
        vc->restart_needed = false;
    } else if (vc->state == VCU_STATE_READY_TO_DRIVE) {
        vc->restart_needed = true;
    }
    vc->state = to;
    vc->entered_ms = in->now_ms;
}

static uint16_t cause_bit(vcu_cause_t cause)
{
    return (uint16_t)(1u << (cause - VCU_CAUSE_SDC_OPEN));
}

/* Leaving Ready to Drive: every cause that applies now, as bits
   (cause - VCU_CAUSE_SDC_OPEN). Faults count only from a VALID mailbox and
   the voltages only with both theirs VALID (bus_view_t), so a mailbox
   timeout stays a Torque cut (ADR 0006). */
static uint16_t leave_causes(const vcu_control_in_t *in, const bus_view_t *bus)
{
    uint16_t c = 0u;

    if (!in->sdc_closed) {
        c |= cause_bit(VCU_CAUSE_SDC_OPEN);
    }
    if (bus->bms_fault) {
        c |= cause_bit(VCU_CAUSE_BMS_FAULT);
    }
    if (bus->inverter_fault) {
        c |= cause_bit(VCU_CAUSE_INVERTER_FAULT);
    }
    if (in->bus_off_latched) {
        c |= cause_bit(VCU_CAUSE_BUS_OFF_LATCHED);
    }
    if (bus->voltages_valid && !bus_at_least(bus, VCU_TS_LOST_PERCENT)) {
        c |= cause_bit(VCU_CAUSE_BUS_VOLTAGE_LOW);
    }
    return c;
}

/* The first cause in causes, in vcu_cause_t order. */
static vcu_cause_t first_cause(uint16_t causes)
{
    unsigned bit = 0u;

    while ((causes & (1u << bit)) == 0u) {
        bit++;
    }
    return (vcu_cause_t)(VCU_CAUSE_SDC_OPEN + bit);
}

/* ------------------------------------------------------------------ */
/* Frames                                                             */
/* ------------------------------------------------------------------ */

/* 0x0C0, every step after STARTUP. torque is what the gates let through,
   0 until the torque path (issue 57). */
static void send_command(vcu_control_t *vc, int16_t torque, vcu_control_out_t *out)
{
    const bool enable = vc->state == VCU_STATE_READY_TO_DRIVE;
    const struct can_control_inv_command_t command = {
        .torque_command = torque,
        .speed_command = 0,
        .direction_command = 1u, /* forward */
        .inverter_enable = enable ? 1u : 0u,
        .inverter_discharge = 0u, /* discharge is hardware-only */
        .speed_mode_enable = 0u,
        .rolling_counter = vc->rolling_counter,
        /* The Parameter itself, never a computed value; 0 would mean the
           inverter's EEPROM limit (can/control.dbc). */
        .torque_limit = (int16_t)vc->config.max_torque,
    };

    (void)can_control_inv_command_pack(out->command, &command, CAN_CONTROL_INV_COMMAND_LENGTH);
    out->send_command = true;
    vc->rolling_counter = (uint8_t)((vc->rolling_counter + 1u) & 0x0Fu);
    vc->enable_sent = enable;
}

static void send_status(vcu_control_t *vc, const vcu_control_in_t *in,
                        const bus_view_t *bus, int16_t torque, vcu_control_out_t *out)
{
    out->send_status = vcu_tx_status_due(in->step_count);
    if (!out->send_status) {
        return;
    }
    const struct can_control_vcu_status_t status = {
        .state = vc->state,
        .apps_implausible = cut_active(out, VCU_CUT_APPS) ? 1u : 0u,
        .brake_sensor_failed = cut_active(out, VCU_CUT_BRAKE_SENSOR) ? 1u : 0u,
        .brake_pedal_cut = cut_active(out, VCU_CUT_BRAKE_PEDAL) ? 1u : 0u,
        .bms_status_timeout = bus->bms_status_valid ? 0u : 1u,
        .bms_limits_timeout = bus->bms_limits_valid ? 0u : 1u,
        .bms_power_timeout = bus->bms_power_valid ? 0u : 1u,
        .inverter_timeout = bus->inverter_valid ? 0u : 1u,
        .timing_fault = cut_active(out, VCU_CUT_TIMING) ? 1u : 0u,
        .bms_fault_reported = bus->bms_fault ? 1u : 0u,
        .inverter_fault_reported = bus->inverter_fault ? 1u : 0u,
        .shutdown_circuit_open = in->sdc_closed ? 0u : 1u,
        .bus_off_latched = in->bus_off_latched ? 1u : 0u,
        .parameters_defaulted = vc->config.params_defaulted ? 1u : 0u,
        .startup_blocked = out->startup_blocked ? 1u : 0u,
        .energy_meter_timeout = bus->em_valid ? 0u : 1u,
        .error_passive = in->error_passive ? 1u : 0u,
        .receive_overflow = in->rx_overflowed ? 1u : 0u,
        .last_reset_not_power_on = vc->config.last_reset_not_power_on ? 1u : 0u,
        .not_dual_bank = vc->config.not_dual_bank ? 1u : 0u,
        .brake_engaged = out->brake_engaged ? 1u : 0u,
        .start_button = in->start_pressed ? 1u : 0u,
        .sound_on = out->sound ? 1u : 0u,
        .enable_sent = vc->enable_sent ? 1u : 0u,
        .torque_sent = torque,
    };
    vcu_tx_build_status(&status, &vc->status_tx, out->status);
}

/* ------------------------------------------------------------------ */
/* The step                                                           */
/* ------------------------------------------------------------------ */

void vcu_control_step(vcu_control_t *vc, const vcu_control_in_t *in,
                      vcu_control_out_t *out)
{
    bus_view_t bus;

    memset(out, 0, sizeof(*out));

    /* 1. The snapshot. */
    read_bus(in, &bus);

    /* 2. The pedal and brake sensors. */
    vcu_sensors_compute(&in->raw, &vc->config.cal, &out->sensors);
    out->samples_ready = all_sampled(&in->raw);
    run_cut(vc, in, out, VCU_CUT_APPS, apps_implausible(&out->sensors), VCU_CUT_PERSIST_MS);
    run_cut(vc, in, out, VCU_CUT_BRAKE_SENSOR, brake_sensor_failed(&out->sensors),
            VCU_CUT_PERSIST_MS);
    run_cut(vc, in, out, VCU_CUT_TIMING, in->timing_fault, 0u);

    /* 3. Brake engaged, and the brake-and-pedal check (EV.4.7) on it. */
    out->brake_engaged = brake_engaged(vc, &out->sensors);
    run_cut(vc, in, out, VCU_CUT_BRAKE_PEDAL,
            out->brake_engaged && higher_travel(&out->sensors) > VCU_TRAVEL_BRAKE_CUT, 0u);
    const bool press = start_press(vc, in);

    /* 4. The state transitions, at most one per step. */
    const uint8_t state_before = vc->state;
    switch (vc->state) {
    case VCU_STATE_STARTUP:
        if (in->startup_ok && in->can_running && out->samples_ready) {
            enter(vc, in, out, &bus, VCU_STATE_GLV_ON, VCU_CAUSE_STARTUP_DONE, 0u);
        } else {
            out->startup_blocked = true;
        }
        break;
    case VCU_STATE_GLV_ON:
        if (in->sdc_closed && bus.bms_valid && bus.inverter_valid && !bus.bms_fault &&
            ts_up(&bus)) {
            enter(vc, in, out, &bus, VCU_STATE_TS_ACTIVE, VCU_CAUSE_TS_UP, 0u);
        }
        break;
    case VCU_STATE_TS_ACTIVE:
        /* Judged on a VALID INV_Voltage only: its Substitute (0 V) is no
           news of a discharged bus. */
        if (valid(in, VCU_RX_INV_VOLTAGE) && bus.bus_dv < VCU_TS_MIN_DV) {
            enter(vc, in, out, &bus, VCU_STATE_GLV_ON, VCU_CAUSE_BUS_DISCHARGED, 0u);
        } else if (press && vc->brake_was_engaged &&
                   start_blockers(vc, in, &bus, out) == 0u) {
            /* EV.10.6: the brake was held before the press. */
            enter(vc, in, out, &bus, VCU_STATE_READY_TO_DRIVE, VCU_CAUSE_START_PROCEDURE, 0u);
        }
        break;
    case VCU_STATE_READY_TO_DRIVE: {
        const uint16_t causes = leave_causes(in, &bus);
        if (causes != 0u) {
            enter(vc, in, out, &bus, VCU_STATE_TS_ACTIVE, first_cause(causes), causes);
        }
        break;
    }
    default:
        break;
    }

    out->state = vc->state;
    out->start_blockers = start_blockers(vc, in, &bus, out);
    vc->brake_was_engaged = out->brake_engaged;

    /* 5. The driver request, from the lower Pedal travel. */
    out->torque_request = driver_request(vc, lower_travel(&out->sensors));

    /* 6. The limiters: an empty slot until the Power limiter (issue 60).
       A limiter only ever lowers the request (EV.3.1.3). */
    int16_t torque = out->torque_request;

    /* 7. The gates: zero torque outside READY_TO_DRIVE, on the first
       0x0C0 after entry, and during any Torque cut. Nothing runs after
       them. */
    if (vc->state != VCU_STATE_READY_TO_DRIVE || state_before != VCU_STATE_READY_TO_DRIVE ||
        out->cuts != 0u) {
        torque = 0;
    }
    out->torque = torque;

    /* 8. Send and set the outputs, in the same step. Nothing runs after. */
    if (vc->state != VCU_STATE_STARTUP) {
        send_command(vc, torque, out);
    }
    if (!vc->lamp_check_done &&
        (uint32_t)(in->now_ms - vc->config.reset_ms) >= VCU_LAMP_CHECK_MS) {
        vc->lamp_check_done = true;
    }
    out->brake_light = out->brake_engaged || brake_sensor_failed(&out->sensors) ||
                       cut_active(out, VCU_CUT_BRAKE_SENSOR);
    out->sound = vc->state == VCU_STATE_READY_TO_DRIVE &&
                 (uint32_t)(in->now_ms - vc->entered_ms) < VCU_SOUND_MS;
    /* The fault LED: on for the lamp check and in STARTUP, solid while a
       restart is needed, blinking while a Torque cut lasts, from its first
       step (ADR 0006). */
    const bool cut = out->cuts != 0u;
    if (cut && !vc->cut_was) {
        vc->blink_from_ms = in->now_ms;
    }
    vc->cut_was = cut;
    const bool blink_on = ((uint32_t)(in->now_ms - vc->blink_from_ms) / VCU_FAULT_BLINK_MS) % 2u == 0u;
    out->fault_led = !vc->lamp_check_done || vc->state == VCU_STATE_STARTUP ||
                     vc->restart_needed || (cut && blink_on);
    /* The test point changes in the step that zeroes the command. */
    out->test_point = cut || vc->restart_needed;
    out->fault_active = out->startup_blocked || out->test_point;
    send_status(vc, in, &bus, out->send_command ? torque : 0, out);
}

const char *vcu_control_state_name(uint8_t state)
{
    switch (state) {
    case VCU_STATE_STARTUP:        return "STARTUP";
    case VCU_STATE_GLV_ON:         return "GLV_ON";
    case VCU_STATE_TS_ACTIVE:      return "TS_ACTIVE";
    case VCU_STATE_READY_TO_DRIVE: return "READY_TO_DRIVE";
    default:                       return "?";
    }
}

const char *vcu_control_save_refusal(bool running, uint8_t state)
{
    if (!running || state != VCU_STATE_GLV_ON) {
        return "only in GLV_ON";
    }
    return NULL;
}

const char *vcu_control_bench_refusal(bool running, uint8_t state)
{
    if (running && state == VCU_STATE_READY_TO_DRIVE) {
        return "the VCU is in READY_TO_DRIVE";
    }
    return NULL;
}
