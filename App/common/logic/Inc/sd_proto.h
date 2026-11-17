/*
 * The SD card's SPI-mode protocol, the pieces that are pure (issue #54):
 * building a command frame and reading what the card answers. The driver
 * that moves the bytes is App/common/Src/sd_spi.c.
 *
 * Reference: SD Physical Layer Simplified Specification, section 7 (SPI
 * mode).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef SD_PROTO_H
#define SD_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#define SD_PROTO_COMMAND_SIZE 6u

/* Builds the 6-byte frame for command index (0..63) with its 32-bit
   argument: start bits, index, argument most significant byte first, then
   the CRC7 and the end bit. */
void sd_proto_command(uint8_t index, uint32_t arg, uint8_t frame[SD_PROTO_COMMAND_SIZE]);

/* The argument CMD8 (SEND_IF_COND) is sent with: 2.7-3.6 V and the check
   pattern 0xAA. */
#define SD_PROTO_CMD8_ARG 0x1AAu

/* True when the four bytes after R7's R1 say the card runs at our supply
   and echoed the check pattern: an SD v2 card we can use. */
bool sd_proto_r7_ok(const uint8_t r7[4]);

/* True when the OCR read by CMD58 has CCS set: an SDHC or SDXC card,
   addressed by block number rather than byte offset. */
bool sd_proto_ocr_block_addressed(const uint8_t ocr[4]);

/* The data response token after a written block: true only for "data
   accepted". A CRC error, a write error, an absent card (0xFF) and a
   stuck line (0x00) are all false. */
bool sd_proto_data_accepted(uint8_t token);

/* The block size every card is used with here. */
#define SD_PROTO_BLOCK_SIZE 512u

/* The argument a read or write command takes for this 512-byte sector. */
uint32_t sd_proto_address(bool block_addressed, uint32_t sector);

#endif /* SD_PROTO_H */
