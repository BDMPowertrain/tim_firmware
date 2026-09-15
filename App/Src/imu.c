/* imu.c — ASM330LHHG1 driver (SPI3, onboard). Everything here is a
   direct implementation of Documentation/asm330lhhg1.pdf: the register
   map (section 9), the SPI read/write protocol (section 5.1.2), the
   FIFO watermark mechanism (section 8), and the sensitivity table
   (section 4.6.1). Comments below cite the specific register/table each
   value comes from rather than re-deriving them — check the datasheet
   directly before changing any of these.

   Data flow: IMU_INT (PB4, EXTI4, configured in CubeMX) fires once the
   FIFO holds IMU_FIFO_WATERMARK samples. HAL_GPIO_EXTI_Callback (ISR
   context) does nothing but wake imuTask via a direct task notification
   — the lightest ISR-to-task handoff FreeRTOS has, no queue or
   semaphore object involved. imuTask then takes spi3Mutex, bursts the
   whole FIFO off SPI3 in one transaction, releases the mutex, and
   demuxes the burst into accel/gyro samples by each entry's tag byte.
   This is what keeps SPI3 (shared with the mezzanine header) idle
   between bursts instead of busy at the sensor's own output rate. */

#include "imu.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cmsis_os2.h"

#include "main.h"
#include "print.h"
#include "spi3_bus.h"

extern SPI_HandleTypeDef hspi3;

/* ---- Register map (Documentation/asm330lhhg1.pdf section 9) ---- */
#define IMU_REG_FIFO_CTRL1       0x07u
#define IMU_REG_FIFO_CTRL2       0x08u
#define IMU_REG_FIFO_CTRL3       0x09u
#define IMU_REG_FIFO_CTRL4       0x0Au
#define IMU_REG_INT1_CTRL        0x0Du
#define IMU_REG_WHO_AM_I         0x0Fu
#define IMU_REG_CTRL1_XL         0x10u
#define IMU_REG_CTRL2_G          0x11u
#define IMU_REG_CTRL3_C          0x12u
#define IMU_REG_FIFO_STATUS1     0x3Au
#define IMU_REG_FIFO_DATA_OUT_TAG 0x78u

#define IMU_WHO_AM_I_VALUE       0x6Bu

/* FIFO_CTRL3 (09h): BDR_GY[3:0]=0100, BDR_XL[3:0]=0100 -> both batched
   into the FIFO at 104 Hz (Table 30). */
#define IMU_FIFO_CTRL3_VAL       0x44u

/* Watermark, in FIFO entries (1 entry = 1 sensor sample = 7 bytes, not
   an accel+gyro pair — Table 26). 20 entries at 104 Hz batching both
   sensors is ~10 accel + ~10 gyro samples, roughly a 100 ms burst
   period. Split across FIFO_CTRL1 (low 8 bits) / FIFO_CTRL2 (bit 0). */
#define IMU_FIFO_WATERMARK       20u

/* FIFO_CTRL4 (0Ah): FIFO_MODE[2:0]=110, continuous mode — newest sample
   overwrites the oldest if we ever fall behind, instead of the FIFO
   just stopping (Table 32). */
#define IMU_FIFO_CTRL4_VAL       0x06u

/* INT1_CTRL (0Dh): bit3 INT1_FIFO_TH — routes the watermark flag to
   IMU_INT (Table 38). */
#define IMU_INT1_CTRL_VAL        0x08u

/* CTRL2_G (11h): ODR_G[3:0]=0100 (104 Hz), FS[1:0]_G=11 (+/-2000 dps),
   FS_125=0, FS_4000=0 (Table 46/47). */
#define IMU_CTRL2_G_VAL          0x4Cu

/* CTRL1_XL (10h): ODR_XL[3:0]=0100 (104 Hz), FS[1:0]_XL=11 (+/-8 g,
   note the family's non-sequential encoding: 00=+/-2g, 01=+/-16g,
   10=+/-4g, 11=+/-8g), LPF2_XL_EN=0 (Table 43/44). */
#define IMU_CTRL1_XL_VAL         0x4Cu

/* FIFO_DATA_OUT_TAG bits [7:3] = TAG_SENSOR_[4:0] (Table 132/133). */
#define IMU_FIFO_TAG_GYRO        0x01u
#define IMU_FIFO_TAG_ACCEL       0x02u

#define IMU_FIFO_WORD_BYTES      7u
#define IMU_FIFO_BURST_MAX_WORDS 32u
#define IMU_SPI_BUF_LEN          (1u + IMU_FIFO_BURST_MAX_WORDS * IMU_FIFO_WORD_BYTES)

/* Bulk SPI transfer scratch. Deliberately static, not stack-local:
   imuTask is the only caller, so there's no reentrancy concern, and
   putting a 225-byte buffer on a task's stack is exactly the mistake
   that overflowed shellTask earlier. */
static uint8_t spiTxBuf[IMU_SPI_BUF_LEN];
static uint8_t spiRxBuf[IMU_SPI_BUF_LEN];

/* Cached latest samples, for imu_get_latest_accel()/imu_get_latest_gyro()
   (the `imu` shell command reads these). Guarded by imuDataMutex, a
   second, separate mutex from spi3Mutex — this one protects plain RAM,
   not a peripheral, but the same rule applies: any code that reads or
   writes latestAccel/latestGyro/haveAccel/haveGyro must hold it, or a
   reader can observe a torn, half-updated sample. */
static osMutexId_t imuDataMutex;
static imu_sample_t latestAccel;
static imu_sample_t latestGyro;
static bool haveAccel;
static bool haveGyro;
static bool hwReady;

static osThreadId_t imuTaskHandle;

static void imu_cs_low(void)
{
    HAL_GPIO_WritePin(SPI3_CS_GPIO_Port, SPI3_CS_Pin, GPIO_PIN_RESET);
}

static void imu_cs_high(void)
{
    HAL_GPIO_WritePin(SPI3_CS_GPIO_Port, SPI3_CS_Pin, GPIO_PIN_SET);
}

/* One full-duplex SPI3 transaction: addr_byte out, total_len bytes
   clocked both ways, result left in spiRxBuf (byte 0 is the dummy
   clocked out while addr_byte went out, so real data starts at [1]).
   Caller must already hold spi3Mutex. */
static void imu_transfer(uint8_t addr_byte, size_t total_len)
{
    spiTxBuf[0] = addr_byte;
    imu_cs_low();
    HAL_SPI_TransmitReceive(&hspi3, spiTxBuf, spiRxBuf, (uint16_t)total_len, HAL_MAX_DELAY);
    imu_cs_high();
}

static void imu_write_reg(uint8_t reg, uint8_t val)
{
    spiTxBuf[0] = reg & 0x7Fu;
    spiTxBuf[1] = val;
    imu_cs_low();
    HAL_SPI_Transmit(&hspi3, spiTxBuf, 2, HAL_MAX_DELAY);
    imu_cs_high();
}

static uint8_t imu_read_reg(uint8_t reg)
{
    imu_transfer(reg | 0x80u, 2);
    return spiRxBuf[1];
}

/* FIFO_STATUS1 (3Ah) + FIFO_STATUS2 (3Bh), auto-incremented in one
   transfer (CTRL3_C's IF_INC defaults to 1 — never touched here).
   DIFF_FIFO[9:0] = number of unread FIFO entries (Table 99/101). */
static uint16_t imu_fifo_pending(void)
{
    imu_transfer(IMU_REG_FIFO_STATUS1 | 0x80u, 3);
    return (uint16_t)spiRxBuf[1] | (((uint16_t)spiRxBuf[2] & 0x03u) << 8);
}

/* WHO_AM_I check, then FIFO/INT1 configuration, then finally enabling
   the sensors (CTRL2_G/CTRL1_XL last, so nothing starts filling the
   FIFO until it's fully configured to receive it). Returns false if the
   IMU never answers correctly — a wiring/hardware problem on this base
   board, not something to guess past. */
static bool imu_hw_init(void)
{
    /* imuTask runs within milliseconds of the MCU leaving reset — well
       inside the ASM330LHHG1's own Ton (35 ms, "time to obtain stable
       operation switching from power-down to normal",
       Documentation/asm330lhhg1.pdf electrical characteristics) if its
       supply is still settling. Retry instead of trusting the first
       attempt: up to 10 tries, 20 ms apart, 200 ms worst case — that's
       what actually fixes this, not the fact that a human happened to
       send a second reset moments later. */
    uint8_t who = 0;
    for (uint32_t attempt = 0; attempt < 10u; attempt++) {
        spi3_bus_lock(osWaitForever);
        who = imu_read_reg(IMU_REG_WHO_AM_I);
        spi3_bus_unlock();

        if (who == IMU_WHO_AM_I_VALUE) {
            break;
        }
        osDelay(20);
    }

    if (who != IMU_WHO_AM_I_VALUE) {
        shell_printf("imu: WHO_AM_I mismatch after retries (got 0x%02X, expected 0x%02X) - check wiring\r\n",
                     who, IMU_WHO_AM_I_VALUE);
        return false;
    }

    spi3_bus_lock(osWaitForever);

    /* SW_RESET (CTRL3_C bit0): reboots every register to its power-on
       default, including FIFO state and INT1_CTRL routing. Required
       because this board has no IMU reset line — an MCU-only reset
       (OpenOCD's or our own NVIC_SystemReset()) never touches the IMU's
       own power, so without this, IMU_INT could still be latched from
       whatever a *previous* firmware session left it in (e.g. an
       earlier boot with a broken SPI config that partially configured
       the FIFO). The bit self-clears; poll it rather than guess a
       delay. */
    imu_write_reg(IMU_REG_CTRL3_C, 0x01u);
    for (uint32_t attempt = 0; attempt < 50u; attempt++) {
        if ((imu_read_reg(IMU_REG_CTRL3_C) & 0x01u) == 0u) {
            break;
        }
    }

    imu_write_reg(IMU_REG_FIFO_CTRL3, IMU_FIFO_CTRL3_VAL);
    imu_write_reg(IMU_REG_FIFO_CTRL1, (uint8_t)(IMU_FIFO_WATERMARK & 0xFFu));
    imu_write_reg(IMU_REG_FIFO_CTRL2, (uint8_t)((IMU_FIFO_WATERMARK >> 8) & 0x01u));
    imu_write_reg(IMU_REG_FIFO_CTRL4, IMU_FIFO_CTRL4_VAL);
    imu_write_reg(IMU_REG_INT1_CTRL, IMU_INT1_CTRL_VAL);
    imu_write_reg(IMU_REG_CTRL2_G, IMU_CTRL2_G_VAL);
    imu_write_reg(IMU_REG_CTRL1_XL, IMU_CTRL1_XL_VAL);

    spi3_bus_unlock();

    hwReady = true;
    shell_printf("imu: init ok (WHO_AM_I=0x%02X)\r\n", who);
    return true;
}

/* Runs once per IMU_INT watermark interrupt. Bounded to a 100 ms wait
   for spi3Mutex rather than blocking forever: if the mezzanine bus is
   held longer than that, skipping this cycle and catching up next
   watermark is better than stalling imuTask (osPriorityAboveNormal)
   indefinitely behind it. */
static void imu_drain_fifo(void)
{
    if (!spi3_bus_lock(100)) {
        return;
    }

    uint16_t pending = imu_fifo_pending();
    if (pending > IMU_FIFO_BURST_MAX_WORDS) {
        pending = IMU_FIFO_BURST_MAX_WORDS;
    }

    if (pending > 0) {
        imu_transfer(IMU_REG_FIFO_DATA_OUT_TAG | 0x80u, 1u + (size_t)pending * IMU_FIFO_WORD_BYTES);
    }

    spi3_bus_unlock();

    if (pending == 0) {
        return;
    }

    osMutexAcquire(imuDataMutex, osWaitForever);
    for (uint16_t i = 0; i < pending; i++) {
        const uint8_t *word = &spiRxBuf[1u + (size_t)i * IMU_FIFO_WORD_BYTES];
        uint8_t sensor = (word[0] >> 3) & 0x1Fu;
        int16_t x = (int16_t)((uint16_t)word[1] | ((uint16_t)word[2] << 8));
        int16_t y = (int16_t)((uint16_t)word[3] | ((uint16_t)word[4] << 8));
        int16_t z = (int16_t)((uint16_t)word[5] | ((uint16_t)word[6] << 8));

        if (sensor == IMU_FIFO_TAG_ACCEL) {
            latestAccel.x = x;
            latestAccel.y = y;
            latestAccel.z = z;
            haveAccel = true;
        } else if (sensor == IMU_FIFO_TAG_GYRO) {
            latestGyro.x = x;
            latestGyro.y = y;
            latestGyro.z = z;
            haveGyro = true;
        }
    }
    osMutexRelease(imuDataMutex);
}

/* Deliberately minimal: no SPI3 access (that's spi3_bus_lock(), which
   can block — an ISR must never do that), just wake imuTask. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == IMU_INT_Pin && imuTaskHandle != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR((TaskHandle_t)imuTaskHandle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void imuTask(void *argument)
{
    (void)argument;

    imuTaskHandle = osThreadGetId();

    const osMutexAttr_t imuDataMutex_attributes = {
        .name = "imuDataMutex",
    };
    imuDataMutex = osMutexNew(&imuDataMutex_attributes);
    if (imuDataMutex == NULL) {
        Error_Handler();
    }

    if (!imu_hw_init()) {
        /* Degrade gracefully: nothing left for this task to do without
           a responding IMU. Don't spin, don't retry forever against
           hardware that isn't there. */
        vTaskSuspend(NULL);
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        imu_drain_fifo();
    }
}

bool imu_get_latest_accel(imu_sample_t *out)
{
    bool ok;

    osMutexAcquire(imuDataMutex, osWaitForever);
    ok = haveAccel;
    if (ok) {
        *out = latestAccel;
    }
    osMutexRelease(imuDataMutex);

    return ok;
}

bool imu_get_latest_gyro(imu_sample_t *out)
{
    bool ok;

    osMutexAcquire(imuDataMutex, osWaitForever);
    ok = haveGyro;
    if (ok) {
        *out = latestGyro;
    }
    osMutexRelease(imuDataMutex);

    return ok;
}

bool imu_fifo_status(uint16_t *pending, bool *watermark_flag)
{
    if (!hwReady) {
        return false;
    }

    spi3_bus_lock(osWaitForever);
    imu_transfer(IMU_REG_FIFO_STATUS1 | 0x80u, 3);
    spi3_bus_unlock();

    *pending = (uint16_t)spiRxBuf[1] | (((uint16_t)spiRxBuf[2] & 0x03u) << 8);
    *watermark_flag = (spiRxBuf[2] & 0x80u) != 0;

    return true;
}
