#ifndef APP_INC_SD_SPI_H
#define APP_INC_SD_SPI_H

/* The microSD card on SPI2, in SPI mode (issue #54). FatFs reaches it
   through each image's targets/<image>/FATFS/Target/user_diskio.c, whose
   USER CODE blocks only call these.

   The card is identified below 400 kHz, as the SD specification requires,
   and then used at 10.625 MHz (SPI2 at 170 MHz / 16). SPI2 cannot go
   below 664 kHz (APB1 is 170 MHz and its slowest prescaler is 256), so
   identification clocks the pins by hand at about 300 kHz; see sd_spi.c.
   Each 512-byte block goes by DMA with the calling task asleep until the
   completion interrupt; short command and response exchanges stay polled.

   One task at a time: the one FatFs caller, the SD logger. FatFs's own
   volume lock (_FS_REENTRANT) keeps it that way. Every call returns within
   a bounded time whatever the card does, including when it is pulled out:
   it never hangs the task. */

#include <stdbool.h>
#include <stdint.h>

/* One sector, as FatFs sees it: one card block. */
#define SD_SPI_SECTOR_SIZE 512u

typedef enum {
    SD_SPI_OK,
    SD_SPI_ERROR,     /* the card did not answer, refused, or a transfer timed out */
    SD_SPI_NOT_READY, /* no card identified since the last error: call sd_spi_init() */
} sd_spi_result_t;

/* Identifies the card from scratch, whatever state it was in. True when a
   usable card answered; the other calls then work until an error. */
bool sd_spi_init(void);

/* True between a successful sd_spi_init() and the next error. */
bool sd_spi_ready(void);

/* Reads or writes count 512-byte sectors starting at sector. An error
   leaves the card not ready, so the next mount identifies it again. */
sd_spi_result_t sd_spi_read(uint8_t *buf, uint32_t sector, uint32_t count);
sd_spi_result_t sd_spi_write(const uint8_t *buf, uint32_t sector, uint32_t count);

/* Waits until the card has finished programming the last block written. */
sd_spi_result_t sd_spi_sync(void);

#endif /* APP_INC_SD_SPI_H */
