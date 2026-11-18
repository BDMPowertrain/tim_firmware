/* cell_monitor.c — the link to the cell monitors, see cell_monitor.h. */

#include "cell_monitor.h"

#include "cmsis_os2.h"

#include "cycle_counter.h"
#include "main.h"
#include "spi_dma.h"

extern SPI_HandleTypeDef hspi3;

/* The wake ping: 914 bytes of 0 at 2.66 MHz hold MOSI low for 2.75 ms
   (SLUAA17A 2.1; the bridge wants 2.5-3 ms). */
#define WAKE_PING_BYTES 914u
/* The bridge's TX buffers: a read comes out 128 bytes at a time. */
#define CHUNK 128u

/* Thread flags on the Control step's task. */
#define FLAG_DMA     0x00000001u
#define FLAG_SPI_RDY 0x00000002u

/* A transfer of at most 914 bytes takes 2.75 ms; anything still going at
   5 ms has failed. */
#define DMA_TIMEOUT_MS 5u
/* A write waits up to 250 us for SPI_RDY (SLUSDS1A Fig 7-17): two ticks,
   the shortest wait the RTOS can give that is surely longer. */
#define WRITE_READY_TICKS 2u
/* nCS low to the first clock, and the last clock to nCS high (t9, t10,
   0.5 us; SLUAA17A asks 2 us around the ping). */
#define NCS_GUARD_US 2u

static const uint8_t zeros[WAKE_PING_BYTES] = {0};
static const uint8_t ones[CHUNK] = {
    [0 ... CHUNK - 1u] = 0xFFu,
};

/* What the last read brought back. The Sweep's response is the longest. */
static uint8_t response[BQ_SWEEP_RESPONSE_LEN];

static volatile osThreadId_t waiter;
static volatile bool dmaFailed;
static uint32_t timeouts;

/* ---- interrupts --------------------------------------------------------- */

void spi_dma_other_done(SPI_HandleTypeDef *hspi, bool failed)
{
    if (hspi->Instance != SPI3) {
        return;
    }
    dmaFailed = failed;
    const osThreadId_t t = waiter;
    if (t != NULL) {
        (void)osThreadFlagsSet(t, FLAG_DMA);
    }
}

/* SPI_RDY's rising edge: the bridge has room for a command, or a buffer of
   the response is ready. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != SPI_RDY_Pin) {
        return;
    }
    const osThreadId_t t = waiter;
    if (t != NULL) {
        (void)osThreadFlagsSet(t, FLAG_SPI_RDY);
    }
}

/* ---- pins and transfers ------------------------------------------------- */

static void delay_us(uint32_t us)
{
    const uint32_t start = cycle_counter_read();
    const uint32_t cycles = (SystemCoreClock / 1000000u) * us;

    while (cycle_counter_read() - start < cycles) {
    }
}

static void select(void)
{
    HAL_GPIO_WritePin(BQ_NCS_GPIO_Port, BQ_NCS_Pin, GPIO_PIN_RESET);
    delay_us(NCS_GUARD_US);
}

static void deselect(void)
{
    delay_us(NCS_GUARD_US);
    HAL_GPIO_WritePin(BQ_NCS_GPIO_Port, BQ_NCS_Pin, GPIO_PIN_SET);
}

static bool spi_rdy_high(void)
{
    return HAL_GPIO_ReadPin(SPI_RDY_GPIO_Port, SPI_RDY_Pin) == GPIO_PIN_SET;
}

/* Moves len bytes by DMA and sleeps until done: tx goes out; rx, if not
   NULL, comes in. */
static bool transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    dmaFailed = false;
    (void)osThreadFlagsClear(FLAG_DMA);

    const HAL_StatusTypeDef started =
        (rx != NULL) ? HAL_SPI_TransmitReceive_DMA(&hspi3, (uint8_t *)tx, rx, (uint16_t)len)
                     : HAL_SPI_Transmit_DMA(&hspi3, (uint8_t *)tx, (uint16_t)len);
    if (started != HAL_OK) {
        return false;
    }
    const uint32_t flags = osThreadFlagsWait(FLAG_DMA, osFlagsWaitAny, DMA_TIMEOUT_MS + 1u);
    if ((flags & osFlagsError) != 0u) {
        (void)HAL_SPI_Abort(&hspi3);
        return false;
    }
    if (rx == NULL) {
        /* What MISO clocked in while sending stays out of the next read. */
        (void)HAL_SPIEx_FlushRxFifo(&hspi3);
    }
    return !dmaFailed;
}

/* One frame with nCS low around it. */
static bool send_frame(const uint8_t *frame, size_t len)
{
    select();
    const bool ok = transfer(frame, NULL, len);
    deselect();
    return ok;
}

/* Waits for an SPI_RDY rising edge seen since FLAG_SPI_RDY was last
   cleared, for up to ticks. */
static bool wait_spi_rdy_edge(uint32_t ticks)
{
    const uint32_t flags = osThreadFlagsWait(FLAG_SPI_RDY, osFlagsWaitAny, ticks);
    return (flags & osFlagsError) == 0u;
}

/* SLUSDS1A 7.3.2.1.2.2.3: nCS low, 8 zero bits, nCS high. Clears the
   bridge's FIFOs after a read that did not finish. */
static void comm_clear(void)
{
    (void)send_frame(zeros, 1u);
}

/* ---- commands ----------------------------------------------------------- */

static bool wake_ping(void)
{
    return send_frame(zeros, WAKE_PING_BYTES);
}

static bool write_command(const bq_command_t *cmd)
{
    (void)osThreadFlagsClear(FLAG_SPI_RDY);
    if (!spi_rdy_high() && !wait_spi_rdy_edge(WRITE_READY_TICKS)) {
        comm_clear();
        return false;
    }
    return send_frame(cmd->frame, cmd->len);
}

/* Whole milliseconds, rounded up, plus one tick for where in the tick the
   wait starts. */
static uint32_t ticks_for_us(uint32_t us)
{
    return (us + 999u) / 1000u + 1u;
}

/* Sends a read and takes in its response, 128 bytes per SPI_RDY. Returns
   true if all of it came in; *len is what did. */
static bool read_command(const bq_command_t *cmd, size_t *len)
{
    const size_t expected = (cmd->response_len < sizeof response) ? cmd->response_len
                                                                  : sizeof response;
    const uint32_t start = osKernelGetTickCount();
    const uint32_t limit = ticks_for_us(bq_read_timeout_us(expected));
    size_t got = 0u;

    *len = 0u;
    (void)osThreadFlagsClear(FLAG_SPI_RDY);
    if (!spi_rdy_high() && !wait_spi_rdy_edge(WRITE_READY_TICKS)) {
        comm_clear();
        return false;
    }
    /* SPI_RDY drops within 5 us of the command's first byte and rises
       again when the first buffer is full, so the edge waited for below is
       always a new one. */
    (void)osThreadFlagsClear(FLAG_SPI_RDY);
    if (!send_frame(cmd->frame, cmd->len)) {
        comm_clear();
        return false;
    }

    while (got < expected) {
        const uint32_t elapsed = osKernelGetTickCount() - start;
        const bool ready = (elapsed < limit) && wait_spi_rdy_edge(limit - elapsed);
        const size_t n = (expected - got < CHUNK) ? expected - got : CHUNK;

        /* Cleared before the buffer is read: SPI_RDY falls before its last
           byte goes out and the next edge may come at once. */
        (void)osThreadFlagsClear(FLAG_SPI_RDY);
        select();
        const bool moved = transfer(ones, &response[got], n);
        deselect();
        if (!moved) {
            comm_clear();
            return false;
        }
        if (!ready) {
            /* Timed out: what the bridge held has been read out above (a
               byte it does not hold reads 0xFF, which ends the parsing);
               clear it for the next command (SLUSDS1A Fig 7-16). */
            *len = got + n;
            timeouts++;
            comm_clear();
            return false;
        }
        got += n;
    }
    *len = got;
    return true;
}

void cell_monitor_run(const bq_plan_t *plan, cell_monitor_result_t *out)
{
    bool ok = true;

    out->resp = response;
    out->len = 0u;
    waiter = osThreadGetId();

    if (plan->wake_ping) {
        ok = wake_ping();
    }
    for (size_t i = 0u; ok && i < plan->count; i++) {
        const bq_command_t *cmd = &plan->cmd[i];
        if (cmd->response_len == 0u) {
            ok = write_command(cmd);
        } else {
            ok = read_command(cmd, &out->len);
        }
    }

    waiter = NULL;
    out->ok = ok;
}

uint32_t cell_monitor_timeouts(void)
{
    return timeouts;
}
