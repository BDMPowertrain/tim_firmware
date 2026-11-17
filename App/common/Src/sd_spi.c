/* sd_spi.c — the microSD card on SPI2, SPI mode (issue #54). See sd_spi.h.

   The sequence is the SD Physical Layer Simplified Specification's SPI
   mode (section 7), the same shape as ChaN's reference MMC/SDC module that
   FatFs comes from: CMD0, CMD8 (SD v2 or v1), ACMD41 until the card leaves
   idle (HCS set for v2), CMD58 for the CCS bit (block or byte addressing),
   CMD16 for a byte-addressed card. The frames and the answers are decoded
   by sd_proto (logic, host-tested); this file moves the bytes.

   Two clocks:
   - Identification below 400 kHz (the specification's limit until the card
     is initialised). SPI2 runs from APB1 at 170 MHz and its slowest
     prescaler gives 664 kHz, and slowing APB1 would move FDCAN, SPI3, I2C1
     and the timers with it. So identification clocks PB13 (SCK) and PB15
     (MOSI) by hand as plain outputs, about 300 kHz, reading PB14 (MISO).
     It runs in the logger's task and may be pre-empted: that only stretches
     a clock phase, which SPI mode allows. It takes a few milliseconds of
     CPU once per mount.
   - Everything after it on SPI2 at 10.625 MHz (CubeMX: prescaler 16).
     Each 512-byte block goes by DMA (DMA1 channel 3 receive, channel 4
     send) while the task sleeps on a thread flag set by the completion
     interrupt (priority 9). Commands, responses, tokens and the busy poll
     are a byte or a few, polled.

   Nothing waits without a limit. A card pulled out reads 0xFF (no response,
   no token) or holds MISO low (busy forever): both end in a timeout below,
   the call returns an error, and the card is marked not ready so the next
   mount identifies it again. */

#include "sd_spi.h"

#include "FreeRTOS.h"
#include "task.h"

#include "cmsis_os2.h"

#include "cycle_counter.h"
#include "main.h"
#include "sd_proto.h"

extern SPI_HandleTypeDef hspi2;

/* SD commands used here (section 7.3.1.3); ACMD41 follows CMD55. */
#define CMD_GO_IDLE_STATE      0u
#define CMD_SEND_IF_COND       8u
#define CMD_SET_BLOCKLEN       16u
#define CMD_READ_SINGLE_BLOCK  17u
#define CMD_WRITE_BLOCK        24u
#define ACMD_SD_SEND_OP_COND   41u
#define CMD_APP_CMD            55u
#define CMD_READ_OCR           58u

#define R1_READY 0x00u
#define R1_IDLE  0x01u
#define R1_NONE  0xFFu /* no card answered; bit 7 set, as in any non-response */

#define ACMD41_HCS 0x40000000u /* "we take SDHC/SDXC" */

#define TOKEN_START_BLOCK 0xFEu

/* Identification clock: under the 400 kHz limit with margin. */
#define SLOW_CLOCK_HZ 300000u

/* Limits. The busy and read-token limits are the specification's write
   (250 ms SDHC) and read (100 ms) timeouts with margin; initialisation is
   its 1 s ACMD41 limit. A 512-byte DMA block takes 0.4 ms at 10.6 MHz. */
#define READY_TIMEOUT_MS 500u
#define TOKEN_TIMEOUT_MS 200u
#define INIT_TIMEOUT_MS  1000u
#define DMA_TIMEOUT_MS   50u
#define BYTE_TIMEOUT_MS  5u

/* Bytes polled back to back before the poll sleeps a tick: a short wait
   costs microseconds, a long one costs no CPU. */
#define POLL_BURST 16u

/* The thread flag the DMA completion sets for the task waiting on it. */
#define DMA_DONE_FLAG 0x00000100u

/* SPI2's pins on GPIOB (targets/<image>/tim.ioc). */
#define SCK_PIN  GPIO_PIN_13
#define MISO_PIN GPIO_PIN_14
#define MOSI_PIN GPIO_PIN_15
#define SCK_POS  13u
#define MOSI_POS 15u
#define MODER_OUTPUT 1u
#define MODER_AF     2u

/* Sent while receiving a block: the card reads MOSI high. In flash. */
static const uint8_t allOnes[SD_PROTO_BLOCK_SIZE] = {
    [0 ... SD_PROTO_BLOCK_SIZE - 1u] = 0xFFu,
};

static bool cardReady;
static bool blockAddressed;
static uint8_t (*xfer)(uint8_t out);

static volatile osThreadId_t dmaWaiter;
static volatile bool dmaFailed;

/* ---- the two clocks ---------------------------------------------------- */

static void spin(uint32_t cycles)
{
    const uint32_t start = cycle_counter_read();
    while ((cycle_counter_read() - start) < cycles) {
    }
}

/* Mode 0, most significant bit first: MOSI set while SCK is low, both
   sides sample on the rising edge. */
static uint8_t slow_xfer(uint8_t out)
{
    const uint32_t half = SystemCoreClock / (2u * SLOW_CLOCK_HZ);
    uint8_t in = 0u;

    for (uint32_t bit = 0u; bit < 8u; bit++) {
        GPIOB->BSRR = ((out & 0x80u) != 0u) ? MOSI_PIN : ((uint32_t)MOSI_PIN << 16);
        out = (uint8_t)(out << 1);
        spin(half);
        GPIOB->BSRR = SCK_PIN;
        in = (uint8_t)((in << 1) | (((GPIOB->IDR & MISO_PIN) != 0u) ? 1u : 0u));
        spin(half);
        GPIOB->BSRR = (uint32_t)SCK_PIN << 16;
    }
    return in;
}

static uint8_t fast_xfer(uint8_t out)
{
    uint8_t in = R1_NONE;

    if (HAL_SPI_TransmitReceive(&hspi2, &out, &in, 1u, BYTE_TIMEOUT_MS) != HAL_OK) {
        return R1_NONE;
    }
    return in;
}

/* SCK and MOSI between plain output and SPI2's alternate function. Only
   the mode bits change: the alternate function number, speed and pull
   CubeMX set stay as they are. MISO stays an alternate-function pin, whose
   level the input register still shows. */
static void set_pin_mode(uint32_t mode)
{
    const uint32_t mask = (3u << (2u * SCK_POS)) | (3u << (2u * MOSI_POS));
    const uint32_t bits = (mode << (2u * SCK_POS)) | (mode << (2u * MOSI_POS));

    /* GPIOB's mode register is shared with other pins: no read-modify-
       write may interleave. */
    taskENTER_CRITICAL();
    GPIOB->MODER = (GPIOB->MODER & ~mask) | bits;
    taskEXIT_CRITICAL();
}

static void use_slow_clock(void)
{
    GPIOB->BSRR = ((uint32_t)SCK_PIN << 16) | MOSI_PIN; /* SCK idle low, MOSI high */
    set_pin_mode(MODER_OUTPUT);
    xfer = slow_xfer;
}

static void use_fast_clock(void)
{
    set_pin_mode(MODER_AF);
    (void)HAL_SPIEx_FlushRxFifo(&hspi2);
    xfer = fast_xfer;
}

/* ---- DMA blocks -------------------------------------------------------- */

/* The HAL's weak callbacks; SPI2 is the only SPI using DMA. A later DMA
   user on another SPI must share these, by its instance. */
static void dma_finished(SPI_HandleTypeDef *hspi, bool failed)
{
    if (hspi->Instance != SPI2) {
        return;
    }
    dmaFailed = failed;
    const osThreadId_t waiter = dmaWaiter;
    if (waiter != NULL) {
        (void)osThreadFlagsSet(waiter, DMA_DONE_FLAG);
    }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    dma_finished(hspi, false);
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    dma_finished(hspi, false);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    dma_finished(hspi, true);
}

/* Moves one block by DMA and sleeps until it is done. rx NULL sends only;
   otherwise rx receives while allOnes goes out. */
static bool dma_block(const uint8_t *tx, uint8_t *rx)
{
    dmaFailed = false;
    dmaWaiter = osThreadGetId();
    (void)osThreadFlagsClear(DMA_DONE_FLAG);

    const HAL_StatusTypeDef started = (rx != NULL)
        ? HAL_SPI_TransmitReceive_DMA(&hspi2, (uint8_t *)allOnes, rx, SD_PROTO_BLOCK_SIZE)
        : HAL_SPI_Transmit_DMA(&hspi2, (uint8_t *)tx, SD_PROTO_BLOCK_SIZE);
    if (started != HAL_OK) {
        dmaWaiter = NULL;
        return false;
    }

    const uint32_t flags = osThreadFlagsWait(DMA_DONE_FLAG, osFlagsWaitAny, DMA_TIMEOUT_MS);
    dmaWaiter = NULL;
    if ((flags & osFlagsError) != 0u) {
        (void)HAL_SPI_Abort(&hspi2);
        return false;
    }
    if (rx == NULL) {
        /* Sending alone leaves what the card clocked back in the receive
           FIFO; the next polled byte must not read it. */
        (void)HAL_SPIEx_FlushRxFifo(&hspi2);
    }
    return !dmaFailed;
}

/* ---- commands ---------------------------------------------------------- */

/* Clocks 0xFF until the card answers it (not busy), or the limit. */
static bool wait_ready(uint32_t timeout_ms)
{
    const uint32_t start = HAL_GetTick();

    for (;;) {
        for (uint32_t i = 0u; i < POLL_BURST; i++) {
            if (xfer(0xFFu) == 0xFFu) {
                return true;
            }
        }
        if ((HAL_GetTick() - start) >= timeout_ms) {
            return false;
        }
        osDelay(1);
    }
}

/* Clocks 0xFF until the card sends something else, or the limit. */
static uint8_t wait_token(uint32_t timeout_ms)
{
    const uint32_t start = HAL_GetTick();

    for (;;) {
        for (uint32_t i = 0u; i < POLL_BURST; i++) {
            const uint8_t token = xfer(0xFFu);
            if (token != 0xFFu) {
                return token;
            }
        }
        if ((HAL_GetTick() - start) >= timeout_ms) {
            return 0xFFu;
        }
        osDelay(1);
    }
}

/* Chip select high, then one more byte so the card lets go of MISO. */
static void deselect(void)
{
    HAL_GPIO_WritePin(SPI2_CS_GPIO_Port, SPI2_CS_Pin, GPIO_PIN_SET);
    (void)xfer(0xFFu);
}

static bool select_card(void)
{
    HAL_GPIO_WritePin(SPI2_CS_GPIO_Port, SPI2_CS_Pin, GPIO_PIN_RESET);
    (void)xfer(0xFFu);
    if (wait_ready(READY_TIMEOUT_MS)) {
        return true;
    }
    deselect();
    return false;
}

/* Selects the card, sends one command and returns R1, the card left
   selected for whatever follows. R1_NONE if the card never got ready or
   never answered. */
static uint8_t command(uint8_t index, uint32_t arg)
{
    uint8_t frame[SD_PROTO_COMMAND_SIZE];

    deselect();
    if (!select_card()) {
        return R1_NONE;
    }
    sd_proto_command(index, arg, frame);
    for (uint32_t i = 0u; i < SD_PROTO_COMMAND_SIZE; i++) {
        (void)xfer(frame[i]);
    }

    /* R1 comes within 8 bytes (NCR); bit 7 set means not yet. */
    uint8_t r1 = R1_NONE;
    for (uint32_t i = 0u; i < 10u && (r1 & 0x80u) != 0u; i++) {
        r1 = xfer(0xFFu);
    }
    return r1;
}

static void read_bytes(uint8_t *out, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        out[i] = xfer(0xFFu);
    }
}

/* ACMD41 until the card leaves the idle state, inside the 1 s from start. */
static bool leave_idle(uint32_t start, uint32_t arg)
{
    while ((HAL_GetTick() - start) < INIT_TIMEOUT_MS) {
        if (command(CMD_APP_CMD, 0u) <= R1_IDLE &&
            command(ACMD_SD_SEND_OP_COND, arg) == R1_READY) {
            return true;
        }
        osDelay(1);
    }
    return false;
}

/* ---- the interface ----------------------------------------------------- */

bool sd_spi_init(void)
{
    bool ok = false;
    bool block = false;

    cardReady = false;
    HAL_GPIO_WritePin(SPI2_CS_GPIO_Port, SPI2_CS_Pin, GPIO_PIN_SET);
    use_slow_clock();

    /* At least 74 clocks with CS and MOSI high while the card powers up. */
    for (uint32_t i = 0u; i < 10u; i++) {
        (void)xfer(0xFFu);
    }

    if (command(CMD_GO_IDLE_STATE, 0u) == R1_IDLE) {
        const uint32_t start = HAL_GetTick();

        if (command(CMD_SEND_IF_COND, SD_PROTO_CMD8_ARG) == R1_IDLE) {
            uint8_t r7[4];
            read_bytes(r7, sizeof(r7));
            if (sd_proto_r7_ok(r7) && leave_idle(start, ACMD41_HCS) &&
                command(CMD_READ_OCR, 0u) == R1_READY) {
                uint8_t ocr[4];
                read_bytes(ocr, sizeof(ocr));
                block = sd_proto_ocr_block_addressed(ocr);
                ok = true;
            }
        } else {
            /* CMD8 refused: an SD v1 card, byte addressed. (MMC, which
               would need CMD1, never comes in a microSD slot.) */
            ok = leave_idle(start, 0u);
        }

        if (ok && !block) {
            ok = command(CMD_SET_BLOCKLEN, SD_PROTO_BLOCK_SIZE) == R1_READY;
        }
    }

    deselect();
    use_fast_clock();

    blockAddressed = block;
    cardReady = ok;
    return ok;
}

bool sd_spi_ready(void)
{
    return cardReady;
}

static sd_spi_result_t finish(bool ok)
{
    deselect();
    if (!ok) {
        cardReady = false;
        return SD_SPI_ERROR;
    }
    return SD_SPI_OK;
}

static bool read_block(uint8_t *buf, uint32_t sector)
{
    if (command(CMD_READ_SINGLE_BLOCK, sd_proto_address(blockAddressed, sector)) != R1_READY) {
        return false;
    }
    if (wait_token(TOKEN_TIMEOUT_MS) != TOKEN_START_BLOCK) {
        return false;
    }
    if (!dma_block(NULL, buf)) {
        return false;
    }
    uint8_t crc[2];
    read_bytes(crc, sizeof(crc)); /* CRC checking is off */
    return true;
}

static bool write_block(const uint8_t *buf, uint32_t sector)
{
    if (command(CMD_WRITE_BLOCK, sd_proto_address(blockAddressed, sector)) != R1_READY) {
        return false;
    }
    (void)xfer(0xFFu); /* one byte gap before the token (NWR) */
    (void)xfer(TOKEN_START_BLOCK);
    if (!dma_block(buf, NULL)) {
        return false;
    }
    (void)xfer(0xFFu); /* CRC, unchecked */
    (void)xfer(0xFFu);
    if (!sd_proto_data_accepted(xfer(0xFFu))) {
        return false;
    }
    /* The card holds MISO low while it programs the block. */
    return wait_ready(READY_TIMEOUT_MS);
}

sd_spi_result_t sd_spi_read(uint8_t *buf, uint32_t sector, uint32_t count)
{
    if (!cardReady) {
        return SD_SPI_NOT_READY;
    }
    bool ok = true;
    for (uint32_t i = 0u; ok && i < count; i++) {
        ok = read_block(buf + i * SD_PROTO_BLOCK_SIZE, sector + i);
    }
    return finish(ok);
}

sd_spi_result_t sd_spi_write(const uint8_t *buf, uint32_t sector, uint32_t count)
{
    if (!cardReady) {
        return SD_SPI_NOT_READY;
    }
    bool ok = true;
    for (uint32_t i = 0u; ok && i < count; i++) {
        ok = write_block(buf + i * SD_PROTO_BLOCK_SIZE, sector + i);
    }
    return finish(ok);
}

sd_spi_result_t sd_spi_sync(void)
{
    if (!cardReady) {
        return SD_SPI_NOT_READY;
    }
    /* Every write already waited for programming; selecting waits once
       more for a card that is still busy. */
    return finish(select_card());
}
