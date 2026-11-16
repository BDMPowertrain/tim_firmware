#ifndef APP_INC_FDCAN_IRQ_H
#define APP_INC_FDCAN_IRQ_H

/* Owns HAL's FDCAN receive and error status callbacks, which are shared
   by both controllers, and hands each to the module that owns that controller:
   control_bus.c for FDCAN1 (the Control bus, both images) and can_if.c
   for FDCAN2 (the DAQ bus, VCU only). Neither needs to know the other
   exists, so an image links only the bus modules it uses. */

#include <stdbool.h>
#include <stdint.h>

#include "main.h"

/* Called in interrupt context with the receive FIFO that has a new frame
   (FDCAN_RX_FIFO0 or FDCAN_RX_FIFO1). Copy or notify only. */
typedef void (*fdcan_irq_rx_fn_t)(FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo);

/* Called in interrupt context with the error status flags that fired
   (FDCAN_IR_EP, FDCAN_IR_EW, FDCAN_IR_BO). Copy or count only. */
typedef void (*fdcan_irq_status_fn_t)(FDCAN_HandleTypeDef *hfdcan, uint32_t status_its);

/* Registers the receive handler for one controller. Call before its
   receive interrupts are enabled. Returns false if both slots are taken. */
bool fdcan_irq_register(FDCAN_HandleTypeDef *hfdcan, fdcan_irq_rx_fn_t handler);

/* Registers the error status handler for a controller that already has
   its receive handler. Call before its error status interrupts are
   enabled. Returns false if the controller is not registered. */
bool fdcan_irq_register_status(FDCAN_HandleTypeDef *hfdcan, fdcan_irq_status_fn_t handler);

#endif /* APP_INC_FDCAN_IRQ_H */
