/* vcu_log.c — the VCU's Data log Records, see vcu_log.h. */

#include "vcu_log.h"

#include <string.h>

#include "control_rx.h"

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

static uint8_t bit(bool on, unsigned n)
{
    return on ? (uint8_t)(1u << n) : 0u;
}

void vcu_log_step_encode(uint8_t out[VCU_LOG_STEP_SIZE], const data_log_step_t *common,
                         const vcu_control_in_t *in, const vcu_control_out_t *decided)
{
    data_log_step_encode(out, common);

    out[12] = (uint8_t)(bit(in->sdc_closed, 0) | bit(in->start_pressed, 1) |
                        bit(in->can_running, 2) | bit(in->bus_off_latched, 3) |
                        bit(in->error_passive, 4) | bit(in->rx_overflowed, 5) |
                        bit(decided->samples_ready, 6) | bit(decided->brake_engaged, 7));
    out[13] = (uint8_t)(bit(decided->limiter.active, 0) |
                        bit(decided->limiter.lower == VCU_CEILING_DISCHARGE, 1) |
                        bit(decided->send_command, 2) | bit(decided->send_fault_clear, 3) |
                        bit(decided->send_status, 4) | bit(decided->brake_light, 5) |
                        bit(decided->fault_led, 6) | bit(decided->pedal_check_due, 7));

    put_u16(&out[14], in->raw.apps[0]);
    put_u16(&out[16], in->raw.apps[1]);
    put_u16(&out[18], in->raw.brake[0]);
    put_u16(&out[20], in->raw.brake[1]);
    put_u16(&out[22], in->raw.supply[0]);
    put_u16(&out[24], in->raw.supply[1]);
    put_u16(&out[26], decided->sensors.apps[0].travel);
    put_u16(&out[28], decided->sensors.apps[1].travel);
    out[30] = (uint8_t)decided->sensors.apps[0].status;
    out[31] = (uint8_t)decided->sensors.apps[1].status;
    put_u16(&out[32], decided->sensors.brake[0].pressure);
    put_u16(&out[34], decided->sensors.brake[1].pressure);
    out[36] = (uint8_t)decided->sensors.brake[0].status;
    out[37] = (uint8_t)decided->sensors.brake[1].status;
    put_u16(&out[38], decided->start_blockers);
    put_u16(&out[40], decided->cuts);
    put_u16(&out[42], (uint16_t)decided->torque_request);
    put_u16(&out[44], (uint16_t)decided->torque);
    put_u16(&out[46], (uint16_t)decided->limiter.torque_ceiling);
    put_u32(&out[48], decided->limiter.power_ceiling_w);
    put_u32(&out[52], (uint32_t)decided->limiter.trim_ppm);
    if (decided->send_command) {
        memcpy(&out[56], decided->command, 8u);
    } else {
        memset(&out[56], 0, 8u);
    }

    out[64] = (uint8_t)VCU_RX_COUNT;
    for (size_t i = 0; i < VCU_RX_COUNT; i++) {
        const mailbox_config_t *config = &control_rx_config[vcu_rx_frames[i]];
        uint8_t *row = &out[VCU_LOG_MAILBOX_AT + 9u * i];
        const uint8_t len = (config->length < MAILBOX_FRAME_MAX) ? config->length
                                                                 : (uint8_t)MAILBOX_FRAME_MAX;

        row[0] = (uint8_t)in->mailboxes[i].state;
        memset(&row[1], 0, MAILBOX_FRAME_MAX);
        memcpy(&row[1], mailbox_read(&in->mailboxes[i], config), len);
    }
}

void vcu_log_map_encode(uint8_t out[VCU_LOG_MAP_SIZE])
{
    out[0] = (uint8_t)VCU_RX_COUNT;
    for (size_t i = 0; i < VCU_RX_COUNT; i++) {
        const mailbox_config_t *config = &control_rx_config[vcu_rx_frames[i]];

        put_u32(&out[1u + 4u * i],
                config->frame_id | (config->extended ? DATA_LOG_ID_EXTENDED : 0u));
    }
}
