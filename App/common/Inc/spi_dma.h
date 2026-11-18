#ifndef APP_INC_SPI_DMA_H
#define APP_INC_SPI_DMA_H

/* The HAL's SPI DMA completion callbacks (HAL_SPI_TxRxCpltCallback,
   HAL_SPI_TxCpltCallback, HAL_SPI_ErrorCallback) are defined once, in
   sd_spi.c, for SPI2 and the SD card, which both images have. A DMA
   transfer on any other SPI is handed on to spi_dma_other_done(), from the
   completion interrupt. The default does nothing; an image with such a
   transfer (the BMS's cell monitors on SPI3, cell_monitor.c) defines its
   own. */

#include <stdbool.h>

#include "main.h"

/* failed: the transfer ended in an SPI or DMA error. Interrupt context. */
void spi_dma_other_done(SPI_HandleTypeDef *hspi, bool failed);

#endif /* APP_INC_SPI_DMA_H */
