/* ram_flash.c — RAM-backed flash adapter for the host tests, see
   ram_flash.h. */

#include "ram_flash.h"

#include <string.h>

#define WORDS (RAM_FLASH_SIZE / FLASH_IF_WORD_SIZE)
#define WORDS_PER_PAGE (FLASH_IF_PAGE_SIZE / FLASH_IF_WORD_SIZE)

typedef enum { ERASED, PROGRAMMED, UNDEFINED } word_state_t;

static uint8_t bytes[RAM_FLASH_SIZE];
static word_state_t state[WORDS];
static bool cutArmed;
static bool powerLost;
static uint32_t opsLeft;
static uint32_t ops;
static uint32_t failAddr;
static uint32_t failCountdown; /* 0: no read fault armed */

static bool in_range(uint32_t addr, uint32_t len)
{
    return addr >= RAM_FLASH_BASE && len <= RAM_FLASH_SIZE
        && addr - RAM_FLASH_BASE <= RAM_FLASH_SIZE - len;
}

/* Counts one operation, with power present; false if this one is cut. */
static bool operation_survives(void)
{
    ops++;
    if (cutArmed) {
        if (opsLeft == 0u) {
            powerLost = true;
            return false;
        }
        opsLeft--;
    }
    return true;
}

static bool ram_read(void *ctx, uint32_t addr, void *out, uint32_t len)
{
    (void)ctx;
    if (!in_range(addr, len)) {
        return false;
    }

    uint32_t off = addr - RAM_FLASH_BASE;
    bool ok = true;

    memcpy(out, &bytes[off], len);
    for (uint32_t w = off / FLASH_IF_WORD_SIZE; len > 0u && w <= (off + len - 1u) / FLASH_IF_WORD_SIZE; w++) {
        if (state[w] == UNDEFINED) {
            ok = false;
        }
    }
    if (failCountdown > 0u && failAddr >= addr && failAddr - addr < len) {
        failCountdown--;
        if (failCountdown == 0u) {
            ok = false;
        }
    }
    return ok;
}

static bool ram_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (!in_range(addr, FLASH_IF_PAGE_SIZE) || (addr - RAM_FLASH_BASE) % FLASH_IF_PAGE_SIZE != 0u) {
        return false;
    }

    uint32_t first = (addr - RAM_FLASH_BASE) / FLASH_IF_WORD_SIZE;

    if (powerLost) {
        return false;
    }

    bool survives = operation_survives();

    for (uint32_t w = first; w < first + WORDS_PER_PAGE; w++) {
        state[w] = survives ? ERASED : UNDEFINED;
    }
    memset(&bytes[first * FLASH_IF_WORD_SIZE], survives ? 0xFF : 0x5A, FLASH_IF_PAGE_SIZE);
    return survives;
}

static bool ram_write(void *ctx, uint32_t addr, const uint8_t data[FLASH_IF_WORD_SIZE])
{
    (void)ctx;
    if (!in_range(addr, FLASH_IF_WORD_SIZE) || addr % FLASH_IF_WORD_SIZE != 0u) {
        return false;
    }

    uint32_t w = (addr - RAM_FLASH_BASE) / FLASH_IF_WORD_SIZE;

    if (powerLost || state[w] != ERASED) {
        return false;
    }
    if (!operation_survives()) {
        state[w] = UNDEFINED;
        memset(&bytes[w * FLASH_IF_WORD_SIZE], 0xA5, FLASH_IF_WORD_SIZE);
        return false;
    }
    memcpy(&bytes[w * FLASH_IF_WORD_SIZE], data, FLASH_IF_WORD_SIZE);
    state[w] = PROGRAMMED;
    return true;
}

static const flash_if_t adapter = {
    .ctx = NULL,
    .read = ram_read,
    .erase = ram_erase,
    .write = ram_write,
};

void ram_flash_reset(void)
{
    memset(bytes, 0xFF, sizeof bytes);
    for (uint32_t w = 0; w < WORDS; w++) {
        state[w] = ERASED;
    }
    ops = 0u;
    failCountdown = 0u;
    ram_flash_power_on();
}

void ram_flash_fail_read(uint32_t addr, uint32_t nth)
{
    failAddr = addr;
    failCountdown = nth;
}

const flash_if_t *ram_flash(void)
{
    return &adapter;
}

void ram_flash_cut_after(uint32_t n)
{
    cutArmed = true;
    opsLeft = n;
}

void ram_flash_power_on(void)
{
    cutArmed = false;
    powerLost = false;
    opsLeft = 0u;
}

uint32_t ram_flash_ops(void)
{
    return ops;
}

void ram_flash_flip(uint32_t addr, uint8_t mask)
{
    bytes[addr - RAM_FLASH_BASE] ^= mask;
}

void ram_flash_peek(uint32_t addr, void *out, uint32_t len)
{
    memcpy(out, &bytes[addr - RAM_FLASH_BASE], len);
}
