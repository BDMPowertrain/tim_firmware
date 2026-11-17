#include "sd_proto.h"

/* CRC7 of the SD Physical Layer specification, section 4.5: polynomial
   x^7 + x^3 + 1, initial value 0, bits taken most significant first. */
static uint8_t crc7(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0u;

    for (uint32_t i = 0u; i < len; i++) {
        for (uint32_t bit = 0u; bit < 8u; bit++) {
            const uint8_t in = (uint8_t)(((data[i] >> (7u - bit)) & 1u) ^ ((crc >> 6) & 1u));
            crc = (uint8_t)((crc << 1) & 0x7Fu);
            if (in != 0u) {
                crc ^= 0x09u;
            }
        }
    }
    return crc;
}

void sd_proto_command(uint8_t index, uint32_t arg, uint8_t frame[SD_PROTO_COMMAND_SIZE])
{
    frame[0] = (uint8_t)(0x40u | (index & 0x3Fu));
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)arg;
    frame[5] = (uint8_t)((crc7(frame, 5u) << 1) | 1u);
}

bool sd_proto_r7_ok(const uint8_t r7[4])
{
    return (r7[2] & 0x0Fu) == ((SD_PROTO_CMD8_ARG >> 8) & 0x0Fu) &&
           r7[3] == (uint8_t)SD_PROTO_CMD8_ARG;
}

bool sd_proto_ocr_block_addressed(const uint8_t ocr[4])
{
    return (ocr[0] & 0x40u) != 0u;
}

bool sd_proto_data_accepted(uint8_t token)
{
    return (token & 0x1Fu) == 0x05u;
}

uint32_t sd_proto_address(bool block_addressed, uint32_t sector)
{
    return block_addressed ? sector : sector * SD_PROTO_BLOCK_SIZE;
}
