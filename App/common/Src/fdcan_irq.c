/* fdcan_irq.c — see fdcan_irq.h. USE_HAL_FDCAN_REGISTER_CALLBACKS is 0
   in both CubeMX projects, so the classic weak callbacks are overridden
   here, once, and dispatched by handle. */

#include "fdcan_irq.h"

#include <stddef.h>

#define FDCAN_IRQ_SLOTS 2u /* FDCAN1 and FDCAN2 */

typedef struct {
    FDCAN_HandleTypeDef *hfdcan;
    fdcan_irq_rx_fn_t    handler;
} fdcan_irq_slot_t;

/* Written from initTask before any FDCAN interrupt is enabled, only read
   in interrupt context after that. */
static fdcan_irq_slot_t slots[FDCAN_IRQ_SLOTS];

bool fdcan_irq_register(FDCAN_HandleTypeDef *hfdcan, fdcan_irq_rx_fn_t handler)
{
    for (size_t i = 0; i < FDCAN_IRQ_SLOTS; i++) {
        if (slots[i].hfdcan == NULL || slots[i].hfdcan == hfdcan) {
            slots[i].hfdcan = hfdcan;
            slots[i].handler = handler;
            return true;
        }
    }
    return false;
}

static void dispatch(FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo)
{
    for (size_t i = 0; i < FDCAN_IRQ_SLOTS; i++) {
        if (slots[i].hfdcan == hfdcan) {
            slots[i].handler(hfdcan, rx_fifo);
            return;
        }
    }
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != 0u) {
        dispatch(hfdcan, FDCAN_RX_FIFO0);
    }
}

void HAL_FDCAN_RxFifo1Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo1ITs)
{
    if ((RxFifo1ITs & FDCAN_IT_RX_FIFO1_NEW_MESSAGE) != 0u) {
        dispatch(hfdcan, FDCAN_RX_FIFO1);
    }
}
