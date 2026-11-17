/* flash_bank2.c — the flash interface on bank 2, see flash_bank2.h.

   Three tasks touch stored data: the shell (Parameter saves, the Event
   log read-out) and the Event writer. Each read, erase and program holds
   flashMutex, so the HAL's flash calls and the ECC flag never see two
   tasks at once. Before flash_bank2_init() (none of the three exists yet)
   there is no mutex and none is needed.

   Erase and program go through the HAL only (ADR 0014). The NMI check
   reads FLASH->ECCR itself: the HAL has no call that returns the failing
   address. */

#include "flash_bank2.h"

#include <string.h>

#include "cmsis_os2.h"
#include "main.h"

#define BANK2_BASE 0x08040000u

/* Set by flash_bank2_nmi() during a read. */
static volatile bool eccFault;

static osMutexId_t flashMutex;

bool flash_bank2_init(void)
{
    static const osMutexAttr_t attr = {
        .name = "flashMutex",
        .attr_bits = osMutexPrioInherit,
    };

    flashMutex = osMutexNew(&attr);
    return flashMutex != NULL;
}

static void lock(void)
{
    if (flashMutex != NULL) {
        (void)osMutexAcquire(flashMutex, osWaitForever);
    }
}

static void unlock(void)
{
    if (flashMutex != NULL) {
        (void)osMutexRelease(flashMutex);
    }
}

static bool in_data_region(uint32_t addr, uint32_t len)
{
    return addr >= FLASH_BANK2_DATA_START && len <= FLASH_BANK2_DATA_END - FLASH_BANK2_DATA_START
        && addr - FLASH_BANK2_DATA_START <= (FLASH_BANK2_DATA_END - FLASH_BANK2_DATA_START) - len;
}

static bool bank2_read(void *ctx, uint32_t addr, void *out, uint32_t len)
{
    (void)ctx;
    if (!in_data_region(addr, len)) {
        return false;
    }

    /* ADDR_ECC and BK_ECC hold the first error until both flags are
       cleared, so an old corrected error must not name the address of this
       read's. */
    lock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCC | FLASH_FLAG_ECCD);
    eccFault = false;
    memcpy(out, (const void *)addr, len);
    __DSB();
    const bool ok = !eccFault;
    unlock();
    return ok;
}

static bool bank2_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (!in_data_region(addr, FLASH_IF_PAGE_SIZE) || (addr - BANK2_BASE) % FLASH_IF_PAGE_SIZE != 0u) {
        return false;
    }

    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks = FLASH_BANK_2,
        .Page = (addr - BANK2_BASE) / FLASH_IF_PAGE_SIZE,
        .NbPages = 1u,
    };
    uint32_t pageError = 0u;
    HAL_StatusTypeDef status = HAL_ERROR;

    lock();
    if (HAL_FLASH_Unlock() == HAL_OK) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_SR_ERRORS);
        status = HAL_FLASHEx_Erase(&erase, &pageError);
        (void)HAL_FLASH_Lock();
    }
    unlock();
    return status == HAL_OK;
}

static bool bank2_write(void *ctx, uint32_t addr, const uint8_t data[FLASH_IF_WORD_SIZE])
{
    (void)ctx;
    if (!in_data_region(addr, FLASH_IF_WORD_SIZE) || addr % FLASH_IF_WORD_SIZE != 0u) {
        return false;
    }

    uint64_t word;
    HAL_StatusTypeDef status = HAL_ERROR;

    memcpy(&word, data, sizeof word);
    lock();
    if (HAL_FLASH_Unlock() == HAL_OK) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_SR_ERRORS);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, word);
        (void)HAL_FLASH_Lock();
    }
    unlock();
    return status == HAL_OK;
}

static const flash_if_t bank2 = {
    .ctx = NULL,
    .read = bank2_read,
    .erase = bank2_erase,
    .write = bank2_write,
};

const flash_if_t *flash_bank2_get(void)
{
    FLASH_OBProgramInitTypeDef ob = {0};

    HAL_FLASHEx_OBGetConfig(&ob);
    return (ob.USERConfig & OB_DBANK_64_BITS) != 0u ? &bank2 : NULL;
}

/* ECCR.ADDR_ECC is the failing double word's byte offset within the bank
   named by BK_ECC (RM0440 3.7.9): checked on the bench by reading a page
   whose program was cut. */
bool flash_bank2_nmi(void)
{
    uint32_t eccr = FLASH->ECCR;

    if ((eccr & FLASH_ECCR_ECCD) == 0u || (eccr & FLASH_ECCR_SYSF_ECC) != 0u
        || (eccr & FLASH_ECCR_BK_ECC) == 0u) {
        return false;
    }

    uint32_t addr = BANK2_BASE + ((eccr & FLASH_ECCR_ADDR_ECC) >> FLASH_ECCR_ADDR_ECC_Pos);

    if (!in_data_region(addr, FLASH_IF_WORD_SIZE)) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD);
    eccFault = true;
    return true;
}
