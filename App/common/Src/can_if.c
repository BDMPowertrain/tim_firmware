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
   latest frame under a mutex, and counts.

   Sending: can_send() pushes onto txQueue (frame_queue.h, drop-oldest) in
   a critical section and wakes daqTask, which takes one frame at a time
   and hands it to the (also 3-deep) hardware TX FIFO. A full hardware
   FIFO keeps the frame in daqTask's hands and retries a few ms later;
   meanwhile the queue keeps only the newest frames. Every 100 ms daqTask
   also publishes, and polls the controller for bus-off. */

#include "can_if.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "cmsis_os2.h"

#include "control_step.h"
#include "daq_retry.h"
#include "data_log_card.h"
#include "data_log_ring.h"
#include "fdcan_irq.h"
#include "frame_queue.h"
#include "main.h"
#include "termination.h"

extern FDCAN_HandleTypeDef hfdcan2;

#define CAN_RX_QUEUE_DEPTH 8u

/* All three hardware send slots, for cancelling every pending send at a
   bus-off. */
#define TX_ALL_SLOTS (FDCAN_TX_BUFFER0 | FDCAN_TX_BUFFER1 | FDCAN_TX_BUFFER2)

/* The publisher's period (issue 36): the DaqStatus frames' cycle time in
   can/daq.dbc. */
#define DAQ_PUBLISH_PERIOD_MS 100u

/* How soon daqTask retries a frame the full hardware FIFO refused: three
   frames go out in well under 1 ms at 2 Mbit/s, so this is only reached
   when nothing ACKs them. */
#define DAQ_TX_RETRY_MS 5u

static osMessageQueueId_t rxQueue;

/* Shared by every sender and daqTask: touched only inside a critical
   section, which holds a push or a pop of one frame and nothing else. */
static frame_queue_t txQueue;
static osSemaphoreId_t txWake; /* given by can_send(), taken by daqTask */

/* daqTask's own. The shell reads the counters as single words. */
static const can_if_publisher_t *publisher;
static daq_retry_t retry;
static uint32_t published;
static bool started;

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

static uint32_t rxCount;
static uint32_t txCount;
static uint32_t rxDropCount;
static uint32_t txRejected;

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

bool can_if_init(const can_if_publisher_t *image_publisher, bool terminate)
{
    publisher = image_publisher;
    rxQueue = osMessageQueueNew(CAN_RX_QUEUE_DEPTH, sizeof(can_frame_t), NULL);
    txWake = osSemaphoreNew(1u, 0u, NULL);
    const osMutexAttr_t mutex_attr = { .name = "can2RxCache" };
    rxCacheMutex = osMutexNew(&mutex_attr);
    if (rxQueue == NULL || txWake == NULL || rxCacheMutex == NULL) {
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
    started = true;
    if (HAL_FDCAN_ActivateNotification(&hfdcan2, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
        return false;
    }

    /* ADM3057EBRWZ STBY: high = standby (default at boot, per CubeMX's
       generated MX_GPIO_Init), low = normal operation
       (Documentation/adm3055e-adm3057e.pdf Table 10, p.15). Then
       termination from its Parameter, out of standby first (issue 52). */
    can_set_standby(false);
    termination_write_stored(TERMINATION_DAQ_BUS, terminate);
    return true;
}

/* The only two touches of txQueue. Each holds one frame copy and the
   ring's indices, a microsecond or so with the scheduler and the
   interrupts at or below the RTOS ceiling (CAN1 included) held off. */
static void tx_queue_push(const can_frame_t *frame)
{
    taskENTER_CRITICAL();
    frame_queue_push(&txQueue, frame);
    taskEXIT_CRITICAL();
    (void)osSemaphoreRelease(txWake); /* already given: daqTask is due anyway */
}

static bool tx_queue_pop(can_frame_t *out)
{
    taskENTER_CRITICAL();
    const bool popped = frame_queue_pop(&txQueue, out);
    taskEXIT_CRITICAL();
    return popped;
}

bool can_send(uint32_t id, const uint8_t *data, uint8_t len,
              bool is_extended, bool is_fd, bool is_brs)
{
    if (is_fd && !fdCapable) {
        txRejected++;
        return false;
    }
    /* Classic frames top out at 8 bytes; without this check a longer
       classic payload would get a DLC code len_to_dlc() only intends for
       FD and be silently rejected deep in HAL_FDCAN_AddMessageToTxFifoQ. */
    if (!is_fd && len > 8u) {
        txRejected++;
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

    tx_queue_push(&frame);
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
    out->terminated = termination_relay(TERMINATION_DAQ_BUS);
    out->fd_capable = fdCapable;
    out->bus_off = (proto.BusOff != 0u);
    out->error_passive = (proto.ErrorPassive != 0u);
    out->last_error_code = proto.LastErrorCode;
    out->tx_error_count = errCounters.TxErrorCnt;
    out->rx_error_count = errCounters.RxErrorCnt;
    out->rx_count = rxCount;
    out->tx_count = txCount;
    out->rx_drop_count = rxDropCount;
    out->tx_queue_dropped = txQueue.dropped;
    out->tx_rejected = txRejected;
    out->published = published;
    out->bus_offs = retry.bus_offs;
    out->restarts = retry.restarts;

    return true;
}

void can_set_standby(bool on)
{
    HAL_GPIO_WritePin(FDCAN2_STB_GPIO_Port, FDCAN2_STB_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    standby = on;
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
        /* The Data log's raw copy (issue 61); an image without the ring
           records nothing. */
        data_log_push_daq(false, &frame, osKernelGetTickCount());

        /* Activity LED, active low per CLAUDE.md: toggling on every
           processed frame gives a visible blink at low traffic and a
           flicker at high, with no extra timer. */
        HAL_GPIO_TogglePin(GPIO_CAN2LED_GPIO_Port, GPIO_CAN2LED_Pin);
    }
}

/* The controller sets CCCR.INIT as it goes bus-off and keeps it set until
   software clears it (RM0440, FDCAN bus-off recovery). FDCAN2 has been
   started by then, so INIT set means bus-off. */
static bool controller_off(void)
{
    return (hfdcan2.Instance->CCCR & FDCAN_CCCR_INIT) != 0u;
}

/* The DAQ publisher: the image's status frame from a copy of the Control
   step's published snapshot. The copy is the step's only contact with the
   DAQ bus, and the step is not the one making it. */
static void publish(void)
{
    control_snapshot_t snap;
    control_step_read(&snap);

    /* events_dropped and allocation_failed wait for the Event log. */
    const daq_status_t status = {
        .overruns = snap.overruns,
        .background_stalled = snap.background_stalled,
        .control_bus_off = snap.bus.health.off,
        .control_bus_latched = snap.bus.health.latched,
        .control_bus_error_passive = snap.bus.health.error_passive,
        .card_state = data_log_card_choice(data_log_state()),
        .records_dropped = data_log_dropped(),
    };
    can_frame_t frame = {
        .id = publisher->status_frame_id,
        .len = publisher->status_frame_len,
        .is_fd = fdCapable, /* can/daq.dbc: StandardCAN_FD */
        .is_brs = fdCapable,
    };
    publisher->build_status(&status, frame.data);
    tx_queue_push(&frame);

    /* The image's own frames, which it builds from its own copy of the
       snapshot. Static: daqTask's alone, about 1 KB. */
    static can_frame_t frames[CAN_IF_PUBLISH_FRAMES_MAX];
    const size_t n = (publisher->build_frames != NULL)
                         ? publisher->build_frames(published, frames, CAN_IF_PUBLISH_FRAMES_MAX)
                         : 0u;
    for (size_t i = 0; i < n && i < CAN_IF_PUBLISH_FRAMES_MAX; i++) {
        if (!fdCapable && frames[i].len > 8u) {
            txRejected++;
            continue;
        }
        frames[i].is_fd = fdCapable; /* can/daq.dbc: StandardCAN_FD */
        frames[i].is_brs = fdCapable;
        tx_queue_push(&frames[i]);
    }
    published++;
}

/* Hands queued frames to the hardware FIFO until it is full or the queue
   is empty. *pending is a frame taken from the queue that the FIFO has
   not yet accepted. False: a frame is still pending. */
static bool drain(can_frame_t *pending, bool *have_pending)
{
    for (;;) {
        if (!*have_pending) {
            if (!tx_queue_pop(pending)) {
                return true;
            }
            *have_pending = true;
        }
        /* Bus-off: hold the frame. The queue meanwhile keeps the newest. */
        if (controller_off()) {
            return false;
        }

        FDCAN_TxHeaderTypeDef hdr;
        can_frame_to_tx_header(pending, &hdr);
        /* The hardware FIFO is only 3 deep: full is normal under load. */
        if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &hdr, pending->data) != HAL_OK) {
            return false;
        }
        *have_pending = false;
        txCount++;
        data_log_push_daq(true, pending, osKernelGetTickCount());
        HAL_GPIO_TogglePin(GPIO_CAN2LED_GPIO_Port, GPIO_CAN2LED_Pin);
    }
}

void daqTask(void *argument)
{
    (void)argument;

    can_frame_t pending;
    bool havePending = false;
    bool blocked = false; /* the hardware FIFO refused the pending frame */
    uint32_t nextPublish = osKernelGetTickCount() + DAQ_PUBLISH_PERIOD_MS;

    for (;;) {
        /* Sleep until the next publish, a frame is queued, or, with the
           hardware FIFO full on a running bus, a short retry. */
        const int32_t untilPublish = (int32_t)(nextPublish - osKernelGetTickCount());
        uint32_t timeout = (untilPublish > 0) ? (uint32_t)untilPublish : 0u;
        if (blocked && !controller_off() && timeout > DAQ_TX_RETRY_MS) {
            timeout = DAQ_TX_RETRY_MS;
        }
        if (timeout > 0u) {
            (void)osSemaphoreAcquire(txWake, timeout);
        }
        const uint32_t now = osKernelGetTickCount();

        const uint32_t busOffsBefore = retry.bus_offs;
        const bool restart = started && daq_retry_update(&retry, controller_off(), now);
        if (retry.bus_offs != busOffsBefore) {
            /* A new bus-off: nothing built before it goes out after the
               restart, as on CAN1. The queue keeps the newest frames. */
            (void)HAL_FDCAN_AbortTxRequest(&hfdcan2, TX_ALL_SLOTS);
            havePending = false;
        }
        if (restart) {
            /* Clearing INIT is the whole restart, as on CAN1
               (control_bus.c): the controller rejoins after 129 bus-idle
               sequences. HAL's own start would refuse, its state never
               having left BUSY. */
            CLEAR_BIT(hfdcan2.Instance->CCCR, FDCAN_CCCR_INIT);
        }

        if ((int32_t)(now - nextPublish) >= 0) {
            publish();
            nextPublish += DAQ_PUBLISH_PERIOD_MS;
            if ((int32_t)(now - nextPublish) >= 0) {
                nextPublish = now + DAQ_PUBLISH_PERIOD_MS; /* late: no catch-up */
            }
        }

        blocked = started && !drain(&pending, &havePending);
    }
}
