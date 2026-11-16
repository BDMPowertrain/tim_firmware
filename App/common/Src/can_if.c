/* can_if.c — FDCAN2 (the DAQ bus) driver, an isolated CAN FD bus on an
   ADM3057EBRWZ transceiver (see Documentation/adm3055e-adm3057e.pdf for
   the STBY polarity cited below). FDCAN1, the Control bus, is
   control_bus.c's and never touched here.

   Data flow: the FDCAN2 receive interrupt (dispatched by fdcan_irq.c)
   drains the hardware RX FIFO0 in a loop — it's only 3 elements deep,
   and the interrupt fires once per new message, so a single-shot read
   would lose frames that arrived while the ISR was already running. Each
   frame goes straight onto rxQueue via xQueueSendFromISR — the native
   FreeRTOS call, not the CMSIS-RTOS2 wrapper, matching imu.c's
   HAL_GPIO_EXTI_Callback pattern. canRxTask drains rxQueue, caches the
   latest frame under a mutex, and counts. canTxTask blocks on txQueue and
   hands frames to the (also 3-deep) hardware TX FIFO. */

#include "can_if.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "cmsis_os2.h"

#include "fdcan_irq.h"
#include "main.h"

extern FDCAN_HandleTypeDef hfdcan2;

#define CAN_QUEUE_DEPTH 8u

static osMessageQueueId_t rxQueue;
static osMessageQueueId_t txQueue;

static osMutexId_t rxCacheMutex;
static can_frame_t latestRx;
static bool        haveRx;
static uint32_t    rxSeq; /* bumped each time canRxTask caches a new frame.
                              haveRx (above) is what actually gates "nothing
                              received yet" -- rxSeq only has to differ when
                              something changes, so it doesn't need
                              special-cased at 0. */

static bool fdCapable;
static bool standby = true;
static bool terminated;

static uint32_t rxCount;
static uint32_t txCount;
static uint32_t rxDropCount;
static uint32_t txDropCount;

/* HAL's DLC codes equal the byte count only for 0-8 bytes; above that
   (FD frames) they're an index, not a length (Table: FDCAN_DLC_BYTES_*
   in stm32g4xx_hal_fdcan.h) — a naive DataLength = len assignment
   silently corrupts any FD frame over 8 bytes. */
static uint32_t len_to_dlc(uint8_t len)
{
    if (len <= 8u)  { return len; }
    if (len <= 12u) { return FDCAN_DLC_BYTES_12; }
    if (len <= 16u) { return FDCAN_DLC_BYTES_16; }
    if (len <= 20u) { return FDCAN_DLC_BYTES_20; }
    if (len <= 24u) { return FDCAN_DLC_BYTES_24; }
    if (len <= 32u) { return FDCAN_DLC_BYTES_32; }
    if (len <= 48u) { return FDCAN_DLC_BYTES_48; }
    return FDCAN_DLC_BYTES_64;
}

static uint8_t dlc_to_len(uint32_t dlc)
{
    static const uint8_t lut[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    return (dlc < 16u) ? lut[dlc] : 64u;
}

static void can_frame_to_tx_header(const can_frame_t *frame, FDCAN_TxHeaderTypeDef *hdr)
{
    hdr->Identifier = frame->id;
    hdr->IdType = frame->is_extended ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
    hdr->TxFrameType = FDCAN_DATA_FRAME;
    hdr->DataLength = len_to_dlc(frame->len);
    hdr->ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr->BitRateSwitch = frame->is_brs ? FDCAN_BRS_ON : FDCAN_BRS_OFF;
    hdr->FDFormat = frame->is_fd ? FDCAN_FD_CAN : FDCAN_CLASSIC_CAN;
    hdr->TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr->MessageMarker = 0;
}

/* Minimal: no SPI/I2C access (nothing here can block), just drain the
   FIFO and hand each frame to the queue. */
static void can_if_rx_isr(FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, rx_fifo) > 0u) {
        FDCAN_RxHeaderTypeDef rxHeader;
        can_frame_t frame = { 0 };

        if (HAL_FDCAN_GetRxMessage(hfdcan, rx_fifo, &rxHeader, frame.data) != HAL_OK) {
            break;
        }

        frame.id = rxHeader.Identifier;
        frame.is_extended = (rxHeader.IdType == FDCAN_EXTENDED_ID);
        frame.is_fd = (rxHeader.FDFormat == FDCAN_FD_CAN);
        frame.is_brs = (rxHeader.BitRateSwitch == FDCAN_BRS_ON);
        frame.len = dlc_to_len(rxHeader.DataLength);

        BaseType_t woken = pdFALSE;
        if (xQueueSendFromISR((QueueHandle_t)rxQueue, &frame, &woken) != pdTRUE) {
            rxDropCount++;
        }
        if (woken != pdFALSE) {
            xHigherPriorityTaskWoken = pdTRUE;
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

bool can_if_init(void)
{
    rxQueue = osMessageQueueNew(CAN_QUEUE_DEPTH, sizeof(can_frame_t), NULL);
    txQueue = osMessageQueueNew(CAN_QUEUE_DEPTH, sizeof(can_frame_t), NULL);
    const osMutexAttr_t mutex_attr = { .name = "can2RxCache" };
    rxCacheMutex = osMutexNew(&mutex_attr);
    if (rxQueue == NULL || txQueue == NULL || rxCacheMutex == NULL) {
        return false;
    }

    /* Read back rather than assume: this reflects whatever
       CubeMX/tim.ioc actually compiled the bus with, so can_send()
       stays correct if a future regen changes it. */
    fdCapable = (hfdcan2.Init.FrameFormat != FDCAN_FRAME_CLASSIC);

    /* Mandatory: with zero filter slots configured, the peripheral
       REJECTS every frame by default unless told otherwise. CubeMX never
       generates this call. Data frames accepted into RX FIFO0 regardless
       of ID; remote frames rejected outright. */
    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan2, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                      FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) {
        return false;
    }
    if (!fdcan_irq_register(&hfdcan2, can_if_rx_isr)) {
        return false;
    }
    if (HAL_FDCAN_Start(&hfdcan2) != HAL_OK) {
        return false;
    }
    if (HAL_FDCAN_ActivateNotification(&hfdcan2, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
        return false;
    }

    /* ADM3057EBRWZ STBY: high = standby (default at boot, per CubeMX's
       generated MX_GPIO_Init), low = normal operation
       (Documentation/adm3055e-adm3057e.pdf Table 10, p.15). Termination
       is deliberately NOT touched here — see can_set_termination(). */
    can_set_standby(false);
    return true;
}

bool can_send(uint32_t id, const uint8_t *data, uint8_t len,
              bool is_extended, bool is_fd, bool is_brs)
{
    if (is_fd && !fdCapable) {
        txDropCount++;
        return false;
    }
    /* Classic frames top out at 8 bytes; without this check a longer
       classic payload would get a DLC code len_to_dlc() only intends for
       FD and be silently rejected deep in HAL_FDCAN_AddMessageToTxFifoQ. */
    if (!is_fd && len > 8u) {
        txDropCount++;
        return false;
    }

    can_frame_t frame = {
        .id = id,
        .is_extended = is_extended,
        .is_fd = is_fd,
        .is_brs = is_fd && is_brs,
    };
    size_t copyLen = (len < sizeof(frame.data)) ? len : sizeof(frame.data);
    memcpy(frame.data, data, copyLen);
    frame.len = (uint8_t)copyLen;

    if (osMessageQueuePut(txQueue, &frame, 0, 0) != osOK) {
        txDropCount++;
        return false;
    }

    return true;
}

bool can_get_new_rx(can_frame_t *out, uint32_t *last_seq)
{
    bool haveNew;

    osMutexAcquire(rxCacheMutex, osWaitForever);
    haveNew = (haveRx && rxSeq != *last_seq);
    if (haveNew) {
        *out = latestRx;
        *last_seq = rxSeq;
    }
    osMutexRelease(rxCacheMutex);

    return haveNew;
}

bool can_get_status(can_status_t *out)
{
    FDCAN_ProtocolStatusTypeDef proto;
    FDCAN_ErrorCountersTypeDef errCounters;

    if (HAL_FDCAN_GetProtocolStatus(&hfdcan2, &proto) != HAL_OK) {
        return false;
    }
    if (HAL_FDCAN_GetErrorCounters(&hfdcan2, &errCounters) != HAL_OK) {
        return false;
    }

    out->standby = standby;
    out->terminated = terminated;
    out->fd_capable = fdCapable;
    out->bus_off = (proto.BusOff != 0u);
    out->error_passive = (proto.ErrorPassive != 0u);
    out->last_error_code = proto.LastErrorCode;
    out->tx_error_count = errCounters.TxErrorCnt;
    out->rx_error_count = errCounters.RxErrorCnt;
    out->rx_count = rxCount;
    out->tx_count = txCount;
    out->rx_drop_count = rxDropCount;
    out->tx_drop_count = txDropCount;

    return true;
}

void can_set_standby(bool on)
{
    HAL_GPIO_WritePin(FDCAN2_STB_GPIO_Port, FDCAN2_STB_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    standby = on;
}

/* Polarity confirmed on hardware (multimeter across CANH/CANL, both
   buses): drive high closes the TLP172AM relay, connecting the two 59R
   resistors in series (~120R) across the bus; drive low leaves it open. */
void can_set_termination(bool on)
{
    HAL_GPIO_WritePin(Can_Term_2_GPIO_Port, Can_Term_2_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    terminated = on;
}

void canRxTask(void *argument)
{
    (void)argument;

    for (;;) {
        can_frame_t frame;
        if (osMessageQueueGet(rxQueue, &frame, NULL, osWaitForever) != osOK) {
            continue;
        }

        osMutexAcquire(rxCacheMutex, osWaitForever);
        latestRx = frame;
        haveRx = true;
        rxSeq++;
        osMutexRelease(rxCacheMutex);

        rxCount++;

        /* Activity LED, active low per CLAUDE.md: toggling on every
           processed frame gives a visible blink at low traffic and a
           flicker at high, with no extra timer. */
        HAL_GPIO_TogglePin(GPIO_CAN2LED_GPIO_Port, GPIO_CAN2LED_Pin);
    }
}

void canTxTask(void *argument)
{
    (void)argument;

    for (;;) {
        can_frame_t frame;
        if (osMessageQueueGet(txQueue, &frame, NULL, osWaitForever) != osOK) {
            continue;
        }

        FDCAN_TxHeaderTypeDef hdr;
        can_frame_to_tx_header(&frame, &hdr);

        /* Hardware TX FIFO is also only 3 deep -- a full FIFO here is a
           normal condition under load, not an edge case. Drop and count
           rather than retry/block. */
        if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &hdr, frame.data) == HAL_OK) {
            txCount++;
            HAL_GPIO_TogglePin(GPIO_CAN2LED_GPIO_Port, GPIO_CAN2LED_Pin);
        } else {
            txDropCount++;
        }
    }
}
