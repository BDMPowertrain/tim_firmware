#include "e2e_p11.h"

#define CRC_BYTE     0u
#define COUNTER_BYTE 1u
#define COUNTER_MASK 0x0Fu

/*
 * CRC-8 SAE J1850, polynomial 0x1D, continued from crc. Profile 11 runs it
 * with a start value of 0x00 and no final inversion (the specification's
 * 0xFF start value and final XOR cancel inside the AUTOSAR CRC routine).
 */
static uint8_t crc8_j1850(uint8_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; bit++) {
            crc = (crc & 0x80u) ? (uint8_t)((uint8_t)(crc << 1) ^ 0x1Du)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* The CRC over the Data ID (low byte first) and every byte after the CRC. */
static uint8_t frame_crc(uint16_t data_id, const uint8_t *frame, size_t len)
{
    const uint8_t id[2] = {(uint8_t)(data_id & 0xFFu), (uint8_t)(data_id >> 8)};
    uint8_t crc = crc8_j1850(0x00u, id, sizeof id);

    return crc8_j1850(crc, &frame[COUNTER_BYTE], len - COUNTER_BYTE);
}

/* 0 to 14, then 0 again. */
static uint8_t next_counter(uint8_t counter)
{
    return (counter >= E2E_P11_COUNTER_MAX) ? 0u : (uint8_t)(counter + 1u);
}

void e2e_p11_protect(e2e_p11_tx_t *tx, uint16_t data_id, uint8_t *frame,
                     size_t len)
{
    if (len < E2E_P11_MIN_LEN) {
        return;
    }

    frame[COUNTER_BYTE] = (uint8_t)((frame[COUNTER_BYTE] & ~COUNTER_MASK) |
                                    (tx->counter & COUNTER_MASK));
    frame[CRC_BYTE] = frame_crc(data_id, frame, len);
    tx->counter = next_counter(tx->counter);
}

e2e_p11_result_t e2e_p11_check(e2e_p11_rx_t *rx, uint16_t data_id,
                               const uint8_t *frame, size_t len)
{
    if (len < E2E_P11_MIN_LEN ||
        frame[CRC_BYTE] != frame_crc(data_id, frame, len)) {
        return E2E_P11_BAD_CRC;
    }

    const uint8_t counter = frame[COUNTER_BYTE] & COUNTER_MASK;
    if (counter > E2E_P11_COUNTER_MAX) {
        return E2E_P11_SKIPPED;
    }

    e2e_p11_result_t result = E2E_P11_OK;
    if (rx->seen) {
        if (counter == rx->last_counter) {
            result = E2E_P11_REPEATED;
        } else if (counter != next_counter(rx->last_counter)) {
            result = E2E_P11_SKIPPED;
        }
    }
    rx->seen = true;
    rx->last_counter = counter;
    return result;
}
