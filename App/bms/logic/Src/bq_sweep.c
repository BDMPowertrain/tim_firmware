/* bq_sweep.c — see bq_sweep.h. */

#include "bq_sweep.h"

#include <string.h>

/* Offsets in a device's 38 bytes. */
#define VCELL1_OFFSET 14u /* VCELLn at (8 - n) * 2 */
#define GPIO1_OFFSET  22u /* GPIOk at 22 + (k - 1) * 2 */

/* LSBs in units of 10 nV, so a reading x LSB / 100000 is millivolts. */
#define CELL_LSB_10NV 19073u /* 190.73 uV, SLUSE81F 6.5 p. 13 */
#define GPIO_LSB_10NV 15259u /* 152.59 uV, same */

size_t bq_sweep_command(uint8_t out[BQ_FRAME_MAX])
{
    return bq_frame_read(out, BQ_STACK_READ, 0u, BQ_SWEEP_REG, BQ_SWEEP_COUNT);
}

/* A big-endian two's complement reading in millivolts, rounded; 0 if it is
   below zero. */
static uint16_t millivolts(const uint8_t *p, uint32_t lsb10nV)
{
    const int16_t raw = (int16_t)(uint16_t)(((unsigned)p[0] << 8) | p[1]);

    if (raw <= 0) {
        return 0u;
    }
    return (uint16_t)(((uint32_t)raw * lsb10nV + 50000u) / 100000u);
}

void bq_sweep_decode(const uint8_t *resp, size_t len, bq_sweep_t *out)
{
    memset(out, 0, sizeof *out);

    const size_t stackLen = (len < BQ_SWEEP_RESPONSE_LEN) ? len : BQ_SWEEP_RESPONSE_LEN;
    size_t pos = 0u;
    bq_response_t r;

    /* The bridge's answer, only ever where the stack's whole one ends. */
    size_t bridgePos = BQ_SWEEP_RESPONSE_LEN;
    if (len > BQ_SWEEP_RESPONSE_LEN
        && bq_response_next(resp, len, &bridgePos, &r) == BQ_RESPONSE_OK && r.dev == 0u
        && r.reg == BQ_SWEEP_BRIDGE_REG && r.len == 1u) {
        out->bridge_read = true;
        out->bridge_fault = r.data[0];
    }

    while (bq_response_next(resp, stackLen, &pos, &r) == BQ_RESPONSE_OK) {
        if (r.dev < 1u || r.dev > BQ_SWEEP_DEVICES || r.reg != BQ_SWEEP_REG
            || r.len != BQ_SWEEP_COUNT) {
            continue;
        }
        const size_t base = (size_t)(r.dev - 1u) * BQ_SWEEP_CELLS_PER_DEVICE;
        for (size_t c = 0u; c < BQ_SWEEP_CELLS_PER_DEVICE; c++) {
            out->cell_mv[base + c] = millivolts(&r.data[VCELL1_OFFSET - c * 2u], CELL_LSB_10NV);
            out->temp_mv[base + c] = millivolts(&r.data[GPIO1_OFFSET + c * 2u], GPIO_LSB_10NV);
        }
        out->devices_read |= (uint16_t)(1u << (r.dev - 1u));
    }
}
