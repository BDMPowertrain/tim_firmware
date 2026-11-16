/*
 * The flash interface stored data is written through (ADR 0014): read,
 * erase one page, program one double word. Two adapters implement it: the
 * real one over the HAL flash functions on bank 2 (App/common/Src/
 * flash_bank2.c) and a RAM-backed one for the host tests
 * (tests/host/common/ram_flash.c).
 *
 * The rules are the STM32G474's in dual-bank mode:
 * - a page is FLASH_IF_PAGE_SIZE bytes and erases to 0xFF;
 * - a double word (FLASH_IF_WORD_SIZE bytes, 8-aligned) is programmed once
 *   after an erase and never again until the next erase;
 * - a double word whose programming or erase was cut by a reset or power
 *   loss is undefined, and reading it can raise an ECC error. The read then
 *   fails rather than handing back the bytes.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdbool.h>
#include <stdint.h>

#define FLASH_IF_PAGE_SIZE 2048u
#define FLASH_IF_WORD_SIZE 8u

typedef struct {
    void *ctx; /* handed back to every call */

    /* Copies len bytes from addr into out. False if any byte read raised
       an ECC error: out then holds whatever came back and must not be
       trusted. */
    bool (*read)(void *ctx, uint32_t addr, void *out, uint32_t len);

    /* Erases the page that starts at addr. False if the erase failed. */
    bool (*erase)(void *ctx, uint32_t addr);

    /* Programs the erased double word at addr (8-aligned). False if the
       program failed. */
    bool (*write)(void *ctx, uint32_t addr, const uint8_t data[FLASH_IF_WORD_SIZE]);
} flash_if_t;

#endif /* FLASH_IF_H */
