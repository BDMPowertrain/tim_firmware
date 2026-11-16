/* control_bus.c — see control_bus.h. FDCAN1 on an ADM3057E transceiver
   (STBY high = standby, Documentation/adm3055e-adm3057e.pdf Table 10,
   p.15). */

#include "control_bus.h"

#include "cmsis_os2.h"

#include "control_step.h"
#include "fdcan_irq.h"
#include "main.h"
#include "masked_copy.h"
#include "print.h"

extern FDCAN_HandleTypeDef hfdcan1;

/* All three send slots, for cancelling every pending send at a bus-off. */
#define TX_ALL_SLOTS (FDCAN_TX_BUFFER0 | FDCAN_TX_BUFFER1 | FDCAN_TX_BUFFER2)

/* The error counters' error-passive level (ISO 11898-1): 128 or more. */
#define ERROR_PASSIVE_COUNT 128u

/* The image's received frames, in its row order: the interrupt's lookup
   table. Written once by control_bus_init() before the interrupts are
   armed. */
static control_rx_t rxFrameOf[CONTROL_BUS_RX_MAX];
static size_t rxRowCount;

/* The interrupt's half of each mailbox: written only by the receive
   interrupt, read only through the masked copy in control_bus_update(). */
static mailbox_rx_t rxLatest[CONTROL_BUS_RX_MAX];

/* The Control step's half, and its copy of the interrupt's half. Static
   rather than on the step's stack. */
static mailbox_rx_t rxSnapshot[CONTROL_BUS_RX_MAX];
static mailbox_t mailboxes[CONTROL_BUS_RX_MAX];

/* Owned by the step. Other tasks see them only through the Control
   step's published snapshot (control_bus_report()). */
static bus_recovery_t recovery;
static uint32_t rxOverflows;

static volatile bool started;
static volatile uint32_t busOffEvents;   /* written only by the interrupt */
static volatile bool     controllerOff;  /* written only by the interrupt */
static volatile uint32_t rxFrames;
static volatile uint32_t rxUnknown;
static volatile uint32_t txSent;
static volatile uint32_t txSkipped;

/* Runs in the FDCAN1 interrupt when the bus-off flag changes. The
   controller sets CCCR.INIT as it goes bus-off and keeps it set until the
   step restarts it (RM0440, FDCAN bus-off recovery), so INIT set here is
   an entry into bus-off and INIT clear is the end of a recovery. Counts
   only: the step does the rest. */
static void control_bus_status_isr(FDCAN_HandleTypeDef *hfdcan, uint32_t status_its)
{
    if ((status_its & FDCAN_IR_BO) == 0u) {
        return;
    }
    const bool entering = (hfdcan->Instance->CCCR & FDCAN_CCCR_INIT) != 0u;
    if (entering) {
        busOffEvents++;
    }
    controllerOff = entering;
}

/* Runs in the FDCAN1 interrupt: drain the FIFO into the mailboxes. The
   FIFO is three frames deep and the interrupt fires once per new frame,
   so read until it is empty. Copies only: no checking, no decoding. */
static void control_bus_rx_isr(FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo)
{
    while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, rx_fifo) > 0u) {
        FDCAN_RxHeaderTypeDef header;
        uint8_t data[MAILBOX_FRAME_MAX];

        if (HAL_FDCAN_GetRxMessage(hfdcan, rx_fifo, &header, data) != HAL_OK) {
            break;
        }

        const int row = control_rx_find(rxFrameOf, rxRowCount, header.Identifier,
                                        header.IdType == FDCAN_EXTENDED_ID);
        if (row < 0) {
            rxUnknown++;
            continue;
        }

        /* Classic CAN: the DLC is the byte count, 0 to 8. */
        const uint8_t len = (header.DataLength <= MAILBOX_FRAME_MAX)
                                ? (uint8_t)header.DataLength
                                : (uint8_t)MAILBOX_FRAME_MAX;
        mailbox_rx_store(&rxLatest[row], data, len, osKernelGetTickCount());
        rxFrames++;
    }
}

static bool config_filter(const control_bus_rx_t *row, uint32_t index)
{
    const mailbox_config_t *config = &control_rx_config[row->frame];
    FDCAN_FilterTypeDef filter = {
        .IdType = config->extended ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID,
        .FilterIndex = index,
        /* A dual-ID filter with the same ID twice is an exact match. */
        .FilterType = FDCAN_FILTER_DUAL,
        .FilterConfig = (row->fifo == CONTROL_BUS_FIFO0) ? FDCAN_FILTER_TO_RXFIFO0
                                                         : FDCAN_FILTER_TO_RXFIFO1,
        .FilterID1 = config->frame_id,
        .FilterID2 = config->frame_id,
    };
    return HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) == HAL_OK;
}

bool control_bus_init(const control_bus_rx_t *rows, size_t row_count)
{
    if (row_count > CONTROL_BUS_RX_MAX) {
        return false;
    }
    for (size_t i = 0; i < row_count; i++) {
        rxFrameOf[i] = rows[i].frame;
    }
    rxRowCount = row_count;

    uint32_t stdIndex = 0;
    uint32_t extIndex = 0;
    for (size_t i = 0; i < row_count; i++) {
        const bool extended = control_rx_config[rows[i].frame].extended;
        uint32_t *index = extended ? &extIndex : &stdIndex;
        const uint32_t slots = extended ? hfdcan1.Init.ExtFiltersNbr
                                        : hfdcan1.Init.StdFiltersNbr;
        if (*index >= slots || !config_filter(&rows[i], *index)) {
            return false;
        }
        (*index)++;
    }

    /* Every ID without a filter is rejected, standard and extended, and
       so is every remote frame. */
    if (HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_REJECT, FDCAN_REJECT,
                                     FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) {
        return false;
    }

    if (!fdcan_irq_register(&hfdcan1, control_bus_rx_isr)
        || !fdcan_irq_register_status(&hfdcan1, control_bus_status_isr)) {
        return false;
    }
    /* The FIFO overflow flags are not interrupts: the step reads them. */
    if (HAL_FDCAN_ActivateNotification(&hfdcan1,
                                       FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO1_NEW_MESSAGE
                                           | FDCAN_IT_BUS_OFF,
                                       0) != HAL_OK) {
        return false;
    }

    /* Out of standby now, so the transceiver has settled well before the
       first step starts CAN1 (spec: at least 5 ms). */
    HAL_GPIO_WritePin(FDCAN1_STB_GPIO_Port, FDCAN1_STB_Pin, GPIO_PIN_RESET);
    return true;
}

bool control_bus_start(void)
{
    started = (HAL_FDCAN_Start(&hfdcan1) == HAL_OK);
    return started;
}

/* Error-passive from the error counters rather than PSR.EP: reading PSR
   resets its last error code, which the shell shows. */
static bool read_error_passive(void)
{
    const uint32_t ecr = hfdcan1.Instance->ECR;
    const uint32_t tec = (ecr & FDCAN_ECR_TEC) >> FDCAN_ECR_TEC_Pos;

    return (ecr & FDCAN_ECR_RP) != 0u || tec >= ERROR_PASSIVE_COUNT;
}

/* A receive FIFO that was full when a frame arrived sets its lost flag.
   Read and clear both (write 1 to clear, so the receive interrupt's own
   flags are untouched). */
static void read_rx_overflow(void)
{
    const uint32_t lost = hfdcan1.Instance->IR & (FDCAN_IR_RF0L | FDCAN_IR_RF1L);

    if (lost != 0u) {
        hfdcan1.Instance->IR = lost;
        rxOverflows++;
    }
}

static void run_recovery(uint32_t now_ms, bool latch_may_clear)
{
    const bus_recovery_input_t in = {
        .bus_off_count = busOffEvents,
        .controller_off = controllerOff,
        .error_passive = read_error_passive(),
        .latch_may_clear = latch_may_clear,
    };
    const bus_recovery_action_t action = bus_recovery_update(&recovery, &in, now_ms);

    if (action.bus_off) {
        /* Every mailbox INVALID in this step, and nothing built before
           the bus-off goes out after the restart. */
        mailbox_force_all_invalid(mailboxes, rxRowCount);
        (void)HAL_FDCAN_AbortTxRequest(&hfdcan1, TX_ALL_SLOTS);
    }
    if (action.restart) {
        /* Clearing INIT is the whole restart: the controller rejoins after
           129 bus-idle sequences, about 2.8 ms at 500 kbit/s. HAL's own
           start would refuse, as its state never left BUSY. */
        CLEAR_BIT(hfdcan1.Instance->CCCR, FDCAN_CCCR_INIT);
    }
}

void control_bus_update(uint32_t now_ms, bool latch_may_clear)
{
    masked_copy(rxSnapshot, rxLatest, rxRowCount * sizeof(rxLatest[0]));

    for (size_t i = 0; i < rxRowCount; i++) {
        (void)mailbox_update(&mailboxes[i], &control_rx_config[rxFrameOf[i]],
                             &rxSnapshot[i], now_ms);
    }

    if (started) {
        read_rx_overflow();
        run_recovery(now_ms, latch_may_clear);
    }
}

const mailbox_t *control_bus_mailbox(size_t row)
{
    return &mailboxes[row];
}

bool control_bus_all_valid(void)
{
    for (size_t i = 0; i < rxRowCount; i++) {
        if (mailboxes[i].state != MAILBOX_VALID) {
            return false;
        }
    }
    return true;
}

control_bus_health_t control_bus_health(void)
{
    return (control_bus_health_t){
        .off = recovery.off,
        .latched = recovery.latched,
        .error_passive = recovery.error_passive,
        .rx_overflowed = (rxOverflows != 0u),
    };
}

control_bus_report_t control_bus_report(void)
{
    return (control_bus_report_t){
        .health = control_bus_health(),
        .recovery = recovery.counters,
        .rx_overflows = rxOverflows,
    };
}

bool control_bus_send(uint32_t frame_id, bool extended, const uint8_t *data,
                      uint8_t len)
{
    /* No free-level pre-check: in queue mode TXFQS.TFFL always reads 0
       (RM0440, FDCAN_TXFQS), so HAL_FDCAN_GetTxFifoFreeLevel() would say
       "full" every time. HAL_FDCAN_AddMessageToTxFifoQ() refuses on
       TXFQS.TFQF instead, which is the real "no free slot". */
    if (!started || recovery.off || len > MAILBOX_FRAME_MAX) {
        txSkipped++;
        return false;
    }

    FDCAN_TxHeaderTypeDef header = {
        .Identifier = frame_id,
        .IdType = extended ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID,
        .TxFrameType = FDCAN_DATA_FRAME,
        .DataLength = len, /* classic CAN: the DLC is the byte count */
        .ErrorStateIndicator = FDCAN_ESI_ACTIVE,
        .BitRateSwitch = FDCAN_BRS_OFF,
        .FDFormat = FDCAN_CLASSIC_CAN,
        .TxEventFifoControl = FDCAN_NO_TX_EVENTS,
        .MessageMarker = 0,
    };
    if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &header, data) != HAL_OK) {
        txSkipped++;
        return false;
    }

    txSent++;
    /* Activity LED, active low: toggles on every frame sent. */
    HAL_GPIO_TogglePin(GPIO_CAN1LED_GPIO_Port, GPIO_CAN1LED_Pin);
    return true;
}

bool control_bus_get_status(control_bus_status_t *out)
{
    FDCAN_ProtocolStatusTypeDef proto;
    FDCAN_ErrorCountersTypeDef errors;

    if (HAL_FDCAN_GetProtocolStatus(&hfdcan1, &proto) != HAL_OK
        || HAL_FDCAN_GetErrorCounters(&hfdcan1, &errors) != HAL_OK) {
        return false;
    }

    out->started = started;
    out->bus_off = (proto.BusOff != 0u);
    out->error_passive = (proto.ErrorPassive != 0u);
    out->last_error_code = proto.LastErrorCode;
    out->tx_error_count = errors.TxErrorCnt;
    out->rx_error_count = errors.RxErrorCnt;
    out->rx_frames = rxFrames;
    out->rx_unknown = rxUnknown;
    out->tx_sent = txSent;
    out->tx_skipped = txSkipped;

    control_snapshot_t snapshot;
    control_step_read(&snapshot);
    out->report = snapshot.bus;
    return true;
}

void control_bus_print_status(void)
{
    control_bus_status_t st;

    if (!control_bus_get_status(&st)) {
        shell_printf("CAN1  status read failed\r\n");
        return;
    }

    const control_bus_report_t *r = &st.report;
    const char *state = !st.started             ? "not started"
                        : r->health.latched     ? "BUS-OFF LATCHED (retry 1/s)"
                        : r->health.off         ? "bus-off, restarting"
                                                : "running";
    shell_printf("CAN1  Control bus, classic: %s\r\n", state);
    shell_printf("     bus_off=%u err_passive=%u last_err=0x%02lX tx_err=%lu rx_err=%lu\r\n",
                 (unsigned)st.bus_off, (unsigned)st.error_passive,
                 (unsigned long)st.last_error_code, (unsigned long)st.tx_error_count,
                 (unsigned long)st.rx_error_count);
    shell_printf("     bus_offs=%lu restarts=%lu err_passives=%lu fifo_overflows=%lu\r\n",
                 (unsigned long)r->recovery.bus_offs, (unsigned long)r->recovery.restarts,
                 (unsigned long)r->recovery.error_passives, (unsigned long)r->rx_overflows);
    shell_printf("     rx=%lu rx_unknown=%lu tx=%lu tx_skipped=%lu\r\n",
                 (unsigned long)st.rx_frames, (unsigned long)st.rx_unknown,
                 (unsigned long)st.tx_sent, (unsigned long)st.tx_skipped);
}
