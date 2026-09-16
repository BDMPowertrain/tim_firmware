/* can_if.c — FDCAN1/FDCAN2 driver, two identical isolated CAN FD buses
   on ADM3057EBRWZ transceivers (see Documentation/adm3055e-adm3057e.pdf
   for the STBY polarity cited below). Both buses share this one module
   via a per-bus context struct instantiated twice rather than
   duplicated code — see can_bus_ctx_t.

   Data flow: HAL_FDCAN_RxFifo0Callback (ISR context) drains the
   hardware RX FIFO0 in a loop — it's only 3 elements deep
   (Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_fdcan.c), and the
   interrupt fires once per new message, so a single-shot read would
   lose frames that arrived while the ISR was already running. Each
   frame goes straight onto that bus's rxQueue via xQueueSendFromISR —
   the native FreeRTOS call, not the CMSIS-RTOS2 wrapper, matching
   imu.c's HAL_GPIO_EXTI_Callback pattern. canRxTask drains rxQueue,
   caches the latest frame under a mutex, and counts — it does not
   forward frames anywhere yet; that's a later VCU-integration step
   (Documentation/tim-rtos-architecture.md order of work). canTxTask
   blocks on that bus's txQueue and hands frames to the (also 3-deep)
   hardware TX FIFO. */

#include "can_if.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "cmsis_os2.h"

#include "main.h"

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;

#define CAN_QUEUE_DEPTH 8u

typedef struct {
    FDCAN_HandleTypeDef *hfdcan;
    const char           *name;
    GPIO_TypeDef         *stb_port;
    uint16_t              stb_pin;
    GPIO_TypeDef         *term_port;
    uint16_t              term_pin;
    GPIO_TypeDef         *led_port;
    uint16_t              led_pin;

    osMessageQueueId_t    rxQueue;
    osMessageQueueId_t    txQueue;

    osMutexId_t           rxCacheMutex;
    can_frame_t           latestRx;
    bool                  haveRx;

    bool                  fdCapable;
    bool                  standby;
    bool                  terminated;

    uint32_t              rxCount;
    uint32_t              txCount;
    uint32_t              rxDropCount;
    uint32_t              txDropCount;
} can_bus_ctx_t;

static can_bus_ctx_t canBuses[CAN_BUS_COUNT] = {
    [CAN_BUS_1] = {
        .hfdcan = &hfdcan1,
        .name = "CAN1",
        .stb_port = FDCAN1_STB_GPIO_Port,   .stb_pin = FDCAN1_STB_Pin,
        .term_port = Can_Term_1_GPIO_Port,  .term_pin = Can_Term_1_Pin,
        .led_port = GPIO_CAN1LED_GPIO_Port, .led_pin = GPIO_CAN1LED_Pin,
    },
    [CAN_BUS_2] = {
        .hfdcan = &hfdcan2,
        .name = "CAN2",
        .stb_port = FDCAN2_STB_GPIO_Port,   .stb_pin = FDCAN2_STB_Pin,
        .term_port = Can_Term_2_GPIO_Port,  .term_pin = Can_Term_2_Pin,
        .led_port = GPIO_CAN2LED_GPIO_Port, .led_pin = GPIO_CAN2LED_Pin,
    },
};

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

void can_if_init(void)
{
    for (int i = 0; i < CAN_BUS_COUNT; i++) {
        can_bus_ctx_t *ctx = &canBuses[i];

        ctx->rxQueue = osMessageQueueNew(CAN_QUEUE_DEPTH, sizeof(can_frame_t), NULL);
        ctx->txQueue = osMessageQueueNew(CAN_QUEUE_DEPTH, sizeof(can_frame_t), NULL);
        const osMutexAttr_t mutex_attr = { .name = ctx->name };
        ctx->rxCacheMutex = osMutexNew(&mutex_attr);
        if (ctx->rxQueue == NULL || ctx->txQueue == NULL || ctx->rxCacheMutex == NULL) {
            Error_Handler();
        }

        /* Read back rather than assume: this reflects whatever
           CubeMX/tim.ioc actually compiled this bus with, so can_send()
           stays correct even if a future regen flips one bus back to
           Classic per the plan's "independent per-bus compile-time
           choice" design. */
        ctx->fdCapable = (ctx->hfdcan->Init.FrameFormat != FDCAN_FRAME_CLASSIC);

        /* Mandatory: with zero filter slots configured (this
           increment's scope — no ID filtering yet), the peripheral
           REJECTS every frame by default unless told otherwise. CubeMX
           never generates this call. Data frames accepted into RX
           FIFO0 regardless of ID; remote frames rejected outright
           (FDCAN_REJECT_REMOTE) since nothing here originates or wants
           them yet. */
        if (HAL_FDCAN_ConfigGlobalFilter(ctx->hfdcan, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                          FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) {
            Error_Handler();
        }
        if (HAL_FDCAN_Start(ctx->hfdcan) != HAL_OK) {
            Error_Handler();
        }
        if (HAL_FDCAN_ActivateNotification(ctx->hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
            Error_Handler();
        }

        /* ADM3057EBRWZ STBY: high = standby (default at boot, per
           CubeMX's generated MX_GPIO_Init), low = normal operation
           (Documentation/adm3055e-adm3057e.pdf Table 10, p.15). Bring
           both buses out of standby by default; termination is
           deliberately NOT touched here — see can_set_termination(). */
        HAL_GPIO_WritePin(ctx->stb_port, ctx->stb_pin, GPIO_PIN_RESET);
        ctx->standby = false;
    }
}

bool can_send(can_bus_t bus, uint32_t id, const uint8_t *data, uint8_t len,
              bool is_extended, bool is_fd, bool is_brs)
{
    can_bus_ctx_t *ctx = &canBuses[bus];

    if (is_fd && !ctx->fdCapable) {
        ctx->txDropCount++;
        return false;
    }
    /* Classic frames top out at 8 bytes; without this check a longer
       classic payload would sail past here, get a DLC code
       len_to_dlc() only intends for FD (9-15, meaning 12-64 bytes),
       and get silently rejected deep in HAL_FDCAN_AddMessageToTxFifoQ
       — same drop counter, much harder to diagnose than catching it
       here. */
    if (!is_fd && len > 8u) {
        ctx->txDropCount++;
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

    if (osMessageQueuePut(ctx->txQueue, &frame, 0, 0) != osOK) {
        ctx->txDropCount++;
        return false;
    }

    return true;
}

bool can_get_latest_rx(can_bus_t bus, can_frame_t *out)
{
    can_bus_ctx_t *ctx = &canBuses[bus];
    bool ok;

    osMutexAcquire(ctx->rxCacheMutex, osWaitForever);
    ok = ctx->haveRx;
    if (ok) {
        *out = ctx->latestRx;
    }
    osMutexRelease(ctx->rxCacheMutex);

    return ok;
}

bool can_get_status(can_bus_t bus, can_status_t *out)
{
    can_bus_ctx_t *ctx = &canBuses[bus];
    FDCAN_ProtocolStatusTypeDef proto;
    FDCAN_ErrorCountersTypeDef errCounters;

    if (HAL_FDCAN_GetProtocolStatus(ctx->hfdcan, &proto) != HAL_OK) {
        return false;
    }
    if (HAL_FDCAN_GetErrorCounters(ctx->hfdcan, &errCounters) != HAL_OK) {
        return false;
    }

    out->standby = ctx->standby;
    out->terminated = ctx->terminated;
    out->fd_capable = ctx->fdCapable;
    out->bus_off = (proto.BusOff != 0u);
    out->error_passive = (proto.ErrorPassive != 0u);
    out->last_error_code = proto.LastErrorCode;
    out->tx_error_count = errCounters.TxErrorCnt;
    out->rx_error_count = errCounters.RxErrorCnt;
    out->rx_count = ctx->rxCount;
    out->tx_count = ctx->txCount;
    out->rx_drop_count = ctx->rxDropCount;
    out->tx_drop_count = ctx->txDropCount;

    return true;
}

void can_set_standby(can_bus_t bus, bool standby)
{
    can_bus_ctx_t *ctx = &canBuses[bus];

    HAL_GPIO_WritePin(ctx->stb_port, ctx->stb_pin, standby ? GPIO_PIN_SET : GPIO_PIN_RESET);
    ctx->standby = standby;
}

/* Polarity of "on" here is a best-effort guess (drive high = presumed
   relay-closed) pending the hardware verification the plan's test
   section calls for: measure resistance across CANH/CANL while
   toggling this and correct this comment (and the WritePin level
   below, if it turns out backwards) once confirmed — the schematic
   strings search couldn't resolve exact relay polarity, only its
   general topology (an opto-isolated TLP172AM switching two 59R
   resistors in series across the bus). */
void can_set_termination(can_bus_t bus, bool on)
{
    can_bus_ctx_t *ctx = &canBuses[bus];

    HAL_GPIO_WritePin(ctx->term_port, ctx->term_pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    ctx->terminated = on;
}

/* Minimal: no SPI/I2C access (nothing here can block), just drain the
   FIFO and hand each frame to its bus's queue — matches imu.c's
   HAL_GPIO_EXTI_Callback in spirit, scaled up for "possibly more than
   one frame per interrupt" instead of a single notification. */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0u) {
        return;
    }

    can_bus_ctx_t *ctx = (hfdcan->Instance == FDCAN1) ? &canBuses[CAN_BUS_1] : &canBuses[CAN_BUS_2];
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0u) {
        FDCAN_RxHeaderTypeDef rxHeader;
        can_frame_t frame = { 0 };

        if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rxHeader, frame.data) != HAL_OK) {
            break;
        }

        frame.id = rxHeader.Identifier;
        frame.is_extended = (rxHeader.IdType == FDCAN_EXTENDED_ID);
        frame.is_fd = (rxHeader.FDFormat == FDCAN_FD_CAN);
        frame.is_brs = (rxHeader.BitRateSwitch == FDCAN_BRS_ON);
        frame.len = dlc_to_len(rxHeader.DataLength);

        BaseType_t woken = pdFALSE;
        if (xQueueSendFromISR((QueueHandle_t)ctx->rxQueue, &frame, &woken) != pdTRUE) {
            ctx->rxDropCount++;
        }
        if (woken != pdFALSE) {
            xHigherPriorityTaskWoken = pdTRUE;
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void canRxTask(void *argument)
{
    can_bus_t bus = (can_bus_t)(uintptr_t)argument;
    can_bus_ctx_t *ctx = &canBuses[bus];

    for (;;) {
        can_frame_t frame;
        if (osMessageQueueGet(ctx->rxQueue, &frame, NULL, osWaitForever) != osOK) {
            continue;
        }

        osMutexAcquire(ctx->rxCacheMutex, osWaitForever);
        ctx->latestRx = frame;
        ctx->haveRx = true;
        osMutexRelease(ctx->rxCacheMutex);

        ctx->rxCount++;
    }
}

void canTxTask(void *argument)
{
    can_bus_t bus = (can_bus_t)(uintptr_t)argument;
    can_bus_ctx_t *ctx = &canBuses[bus];

    for (;;) {
        can_frame_t frame;
        if (osMessageQueueGet(ctx->txQueue, &frame, NULL, osWaitForever) != osOK) {
            continue;
        }

        FDCAN_TxHeaderTypeDef hdr;
        can_frame_to_tx_header(&frame, &hdr);

        /* Hardware TX FIFO is also only 3 deep -- a full FIFO here is
           a normal condition under load, not an edge case. Drop and
           count rather than retry/block, matching logger_post()'s
           established "never stall the producer" rule. */
        if (HAL_FDCAN_AddMessageToTxFifoQ(ctx->hfdcan, &hdr, frame.data) == HAL_OK) {
            ctx->txCount++;
        } else {
            ctx->txDropCount++;
        }
    }
}