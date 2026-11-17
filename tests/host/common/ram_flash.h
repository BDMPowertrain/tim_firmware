/*
 * RAM-backed flash adapter for the host tests: a flash_if_t over a buffer
 * that keeps the rules in flash_if.h, and can lose power.
 *
 * Every erase and every double-word program is one operation. After
 * ram_flash_cut_after(n), n more operations succeed; the next one is cut:
 * the page it erased or the double word it programmed is left undefined
 * (reading any of it fails, as an ECC error would), and from then on every
 * erase and program fails, until ram_flash_power_on().
 */
#ifndef RAM_FLASH_H
#define RAM_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_if.h"

/* Covers the bank 2 data regions: the two Parameter copies and the Event
   log (ADR 0014). */
#define RAM_FLASH_BASE 0x08040000u
#define RAM_FLASH_SIZE 0x12000u

/* Erases everything and powers on with no cut armed. */
void ram_flash_reset(void);

const flash_if_t *ram_flash(void);

/* Arms a cut: n more operations succeed, the one after is cut. */
void ram_flash_cut_after(uint32_t n);

/* Power is back: no cut armed, erase and program work again. Undefined
   words stay undefined until erased. */
void ram_flash_power_on(void);

/* Erases and programs done since ram_flash_reset(), the cut one included. */
uint32_t ram_flash_ops(void);

/* The nth read (1 for the first) that covers addr fails, as a marginal
   word might on one read and not another. Cleared by ram_flash_reset(). */
void ram_flash_fail_read(uint32_t addr, uint32_t nth);

/* Flips bits of one programmed byte, as a corruption the ECC misses. */
void ram_flash_flip(uint32_t addr, uint8_t mask);

/* Reads raw bytes regardless of state, for comparing two moments. */
void ram_flash_peek(uint32_t addr, void *out, uint32_t len);

#endif /* RAM_FLASH_H */
