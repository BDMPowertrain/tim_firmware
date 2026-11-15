/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * @file    user_diskio.c
  * @brief   This file includes a diskio driver skeleton to be completed by the user.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
 /* USER CODE END Header */

#ifdef USE_OBSOLETE_USER_CODE_SECTION_0
/*
 * Warning: the user section 0 is no more in use (starting from CubeMx version 4.16.0)
 * To be suppressed in the future.
 * Kept to ensure backward compatibility with previous CubeMx versions when
 * migrating projects.
 * User code previously added there should be copied in the new user sections before
 * the section contents can be deleted.
 */
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */
#endif

/* USER CODE BEGIN DECL */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include "ff_gen_drv.h"

#include "cmsis_os2.h"
#include "main.h"

extern SPI_HandleTypeDef hspi2;

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* microSD-over-SPI, hand-written against the FatFS diskio shape since
   CubeMX only ships SDIO/SDMMC-peripheral drivers, not SPI ones. Follows
   the standard SD Physical Layer SPI-mode sequence (same shape as
   ChaN's widely-used reference MMC/SDC module, which FatFS itself is
   written by): CMD0 -> CMD8 (probe SDv2 vs SDv1/MMC) -> ACMD41 poll
   (HCS set for SDv2) -> CMD58 (read OCR for the CCS/block-addressing
   bit). Called from loggerTask (via f_mount/f_open/f_write), never from
   shellTask -- see App/common/Src/logger.c. */

/* Commands, already in SPI-mode framing (0x40 | command index) --
   sd_send_cmd() sends these bytes as-is. ACMD41 is not a real command
   on its own; every call site sends CMD55 (APP_CMD) immediately before
   it, per spec -- there's no automatic prefixing here, so it's always
   visible at the call site. */
#define CMD0    (0x40u + 0u)   /* GO_IDLE_STATE */
#define CMD1    (0x40u + 1u)   /* SEND_OP_COND (MMC) */
#define CMD8    (0x40u + 8u)   /* SEND_IF_COND */
#define CMD12   (0x40u + 12u)  /* STOP_TRANSMISSION */
#define CMD16   (0x40u + 16u)  /* SET_BLOCKLEN */
#define CMD17   (0x40u + 17u)  /* READ_SINGLE_BLOCK */
#define CMD24   (0x40u + 24u)  /* WRITE_BLOCK */
#define CMD55   (0x40u + 55u)  /* APP_CMD */
#define CMD58   (0x40u + 58u)  /* READ_OCR */
#define ACMD41  (0x40u + 41u)  /* SD_SEND_OP_COND, under APP_CMD */

/* Card type bits, set by USER_initialize() and consulted by
   USER_read()/USER_write() to decide byte vs. block addressing. */
#define CT_MMC   0x01u
#define CT_SD1   0x02u  /* SD v1: byte addressing, no CMD8 */
#define CT_SD2   0x04u  /* SD v2: CMD8-probed; byte or block per CCS */
#define CT_BLOCK 0x08u  /* SDHC/SDXC: sector number *is* the address */

#define SD_SECTOR_SIZE 512u

/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;
static BYTE CardType;

static void sd_cs_low(void)
{
    HAL_GPIO_WritePin(SPI2_CS_GPIO_Port, SPI2_CS_Pin, GPIO_PIN_RESET);
}

static void sd_cs_high(void)
{
    HAL_GPIO_WritePin(SPI2_CS_GPIO_Port, SPI2_CS_Pin, GPIO_PIN_SET);
}

static BYTE sd_spi_rx(void)
{
    BYTE tx = 0xFFu;
    BYTE rx = 0xFFu;
    HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1, HAL_MAX_DELAY);
    return rx;
}

static void sd_spi_tx(BYTE d)
{
    HAL_SPI_Transmit(&hspi2, &d, 1, HAL_MAX_DELAY);
}

static void sd_spi_rx_block(BYTE *buff, UINT len)
{
    memset(buff, 0xFFu, len);
    HAL_SPI_TransmitReceive(&hspi2, buff, buff, (uint16_t)len, HAL_MAX_DELAY);
}

static void sd_spi_tx_block(const BYTE *buff, UINT len)
{
    HAL_SPI_Transmit(&hspi2, (uint8_t *)buff, (uint16_t)len, HAL_MAX_DELAY);
}

/* Deselect and give the card one extra byte of clock (0xFF) with CS
   high, per spec, so it fully releases MISO before the next command. */
static void sd_deselect(void)
{
    sd_cs_high();
    (void)sd_spi_rx();
}

/* Polls for 0xFF (card ready / not busy) up to timeout_ms. Used both
   after CS goes low (select) and after a write's data response, where
   the card holds MISO low while it programs flash internally -- that
   can legitimately take tens of ms, hence a real timeout rather than a
   fixed retry count. osDelay(1), not HAL_Delay(), since this always
   runs from loggerTask's context (CLAUDE.md: no HAL_Delay in a task). */
static int sd_wait_ready(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    do {
        if (sd_spi_rx() == 0xFFu) {
            return 1;
        }
        osDelay(1);
    } while ((HAL_GetTick() - start) < timeout_ms);

    return 0;
}

static int sd_select(void)
{
    sd_cs_low();
    if (sd_wait_ready(500)) {
        return 1;
    }
    sd_deselect();
    return 0;
}

/* Sends a command packet (6 bytes: command, 4-byte argument big-endian,
   CRC) and returns the R1 response byte. CMD0 and CMD8 are the only two
   commands that need a real CRC7 in SPI mode (every other command's CRC
   is ignored by default, CRC checking off) -- both arguments here are
   fixed (CMD0's argument is always 0, CMD8's is always 0x1AA), so their
   CRC bytes are the well-known constants rather than a computed CRC7,
   same as every reference SD-SPI driver. */
static BYTE sd_send_cmd(BYTE cmd, DWORD arg)
{
    BYTE crc;
    BYTE res;
    BYTE packet[6];

    if (cmd == CMD0) {
        crc = 0x95u;
    } else if (cmd == CMD8) {
        crc = 0x87u;
    } else {
        crc = 0x01u; /* stop bit only; CRC checking is off */
    }

    packet[0] = cmd;
    packet[1] = (BYTE)(arg >> 24);
    packet[2] = (BYTE)(arg >> 16);
    packet[3] = (BYTE)(arg >> 8);
    packet[4] = (BYTE)arg;
    packet[5] = crc;

    sd_cs_low();
    sd_wait_ready(500);
    sd_spi_tx_block(packet, sizeof(packet));

    /* R1 arrives within 8 bytes; 0x80 set means "no response yet". */
    uint32_t tries = 10u;
    do {
        res = sd_spi_rx();
    } while ((res & 0x80u) != 0u && --tries > 0u);

    return res;
}

/* Waits for the data start token (0xFE), then reads len bytes plus the
   2-byte trailing CRC (unchecked -- CRC is off for data too). */
static int sd_rx_data_block(BYTE *buff, UINT len)
{
    BYTE token;
    uint32_t start = HAL_GetTick();

    do {
        token = sd_spi_rx();
    } while (token == 0xFFu && (HAL_GetTick() - start) < 200u);

    if (token != 0xFEu) {
        return 0;
    }

    sd_spi_rx_block(buff, len);

    BYTE crc[2];
    sd_spi_rx_block(crc, sizeof(crc));

    return 1;
}

/* Sends one data block: start token, 512 bytes, 2 dummy CRC bytes, then
   reads the data-response token (bits [4:1] == 0b0101 means accepted)
   and waits for the card to finish programming (sd_wait_ready). */
static int sd_tx_data_block(const BYTE *buff, BYTE token)
{
    BYTE dummy_crc[2] = { 0xFFu, 0xFFu };

    if (!sd_wait_ready(500)) {
        return 0;
    }

    sd_spi_tx(token);
    if (token != 0xFDu) { /* 0xFD = stop token, no payload follows */
        sd_spi_tx_block(buff, SD_SECTOR_SIZE);
        sd_spi_tx_block(dummy_crc, sizeof(dummy_crc));

        BYTE resp = sd_spi_rx();
        if ((resp & 0x1Fu) != 0x05u) {
            return 0;
        }
        if (!sd_wait_ready(500)) {
            return 0;
        }
    }

    return 1;
}

/* USER CODE END DECL */

/* Private function prototypes -----------------------------------------------*/
DSTATUS USER_initialize (BYTE pdrv);
DSTATUS USER_status (BYTE pdrv);
DRESULT USER_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
#if _USE_WRITE == 1
  DRESULT USER_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
  DRESULT USER_ioctl (BYTE pdrv, BYTE cmd, void *buff);
#endif /* _USE_IOCTL == 1 */

Diskio_drvTypeDef  USER_Driver =
{
  USER_initialize,
  USER_status,
  USER_read,
#if  _USE_WRITE
  USER_write,
#endif  /* _USE_WRITE == 1 */
#if  _USE_IOCTL == 1
  USER_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_initialize (
	BYTE pdrv           /* Physical drive nmuber to identify the drive */
)
{
  /* USER CODE BEGIN INIT */
    (void)pdrv;

    Stat = STA_NOINIT;

    BYTE dummy[10];
    memset(dummy, 0xFFu, sizeof(dummy));

    /* >=74 clock pulses with CS high and MOSI high, so the card can
       finish its own power-up before it sees a command. */
    sd_cs_high();
    sd_spi_tx_block(dummy, sizeof(dummy));

    BYTE type = 0;
    BYTE res = sd_send_cmd(CMD0, 0);

    if (res == 0x01u) {
        uint32_t start = HAL_GetTick();

        if (sd_send_cmd(CMD8, 0x1AAu) == 0x01u) {
            /* Card echoes the check pattern back in the trailing 4
               bytes of R7 if it supports the 2.7-3.6V range we asked
               for -- SD v2. */
            BYTE r7[4];
            sd_spi_rx_block(r7, sizeof(r7));

            if (r7[2] == 0x01u && r7[3] == 0xAAu) {
                while ((HAL_GetTick() - start) < 1000u) {
                    if (sd_send_cmd(CMD55, 0) <= 1u &&
                        sd_send_cmd(ACMD41, 0x40000000u) == 0x00u) {
                        break;
                    }
                    osDelay(1);
                }

                if ((HAL_GetTick() - start) < 1000u) {
                    BYTE ocr[4];
                    if (sd_send_cmd(CMD58, 0) == 0x00u) {
                        sd_spi_rx_block(ocr, sizeof(ocr));
                        type = CT_SD2 | ((ocr[0] & 0x40u) ? CT_BLOCK : 0u);
                    }
                }
            }
        } else {
            /* CMD8 rejected: either an old SD v1 card, or an MMC. Try
               ACMD41 (SD path) before falling back to CMD1 (MMC). */
            BYTE cmd;
            if (sd_send_cmd(CMD55, 0) <= 1u && sd_send_cmd(ACMD41, 0) <= 1u) {
                type = CT_SD1;
                cmd = ACMD41;
            } else {
                type = CT_MMC;
                cmd = CMD1;
            }

            while ((HAL_GetTick() - start) < 1000u) {
                if (cmd == ACMD41) {
                    if (sd_send_cmd(CMD55, 0) <= 1u && sd_send_cmd(ACMD41, 0) == 0x00u) {
                        break;
                    }
                } else if (sd_send_cmd(cmd, 0) == 0x00u) {
                    break;
                }
                osDelay(1);
            }

            if ((HAL_GetTick() - start) >= 1000u) {
                type = 0;
            } else if (sd_send_cmd(CMD16, SD_SECTOR_SIZE) != 0x00u) {
                /* Byte-addressed card that won't accept a fixed 512-byte
                   block length -- not usable through this driver. */
                type = 0;
            }
        }
    }

    sd_deselect();
    CardType = type;

    if (type != 0u) {
        Stat &= ~STA_NOINIT;
    }

    return Stat;
  /* USER CODE END INIT */
}

/**
  * @brief  Gets Disk Status
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_status (
	BYTE pdrv       /* Physical drive number to identify the drive */
)
{
  /* USER CODE BEGIN STATUS */
    (void)pdrv;
    return Stat;
  /* USER CODE END STATUS */
}

/**
  * @brief  Reads Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT USER_read (
	BYTE pdrv,      /* Physical drive nmuber to identify the drive */
	BYTE *buff,     /* Data buffer to store read data */
	DWORD sector,   /* Sector address in LBA */
	UINT count      /* Number of sectors to read */
)
{
  /* USER CODE BEGIN READ */
    (void)pdrv;

    if ((Stat & STA_NOINIT) != 0u) {
        return RES_NOTRDY;
    }

    /* SDHC/SDXC (CT_BLOCK) address by block number directly; SDSC/MMC
       address by byte offset, hence the *512. One CMD17 (single-block
       read) per sector -- simplest correct approach; CMD18
       (multi-block) is an optimization for later, not needed at our
       write rate. */
    DWORD addr = (CardType & CT_BLOCK) ? sector : (sector * SD_SECTOR_SIZE);

    for (UINT i = 0; i < count; i++) {
        if (sd_send_cmd(CMD17, addr) != 0x00u) {
            sd_deselect();
            return RES_ERROR;
        }
        if (!sd_rx_data_block(buff + i * SD_SECTOR_SIZE, SD_SECTOR_SIZE)) {
            sd_deselect();
            return RES_ERROR;
        }
        addr += (CardType & CT_BLOCK) ? 1u : SD_SECTOR_SIZE;
    }

    sd_deselect();
    return RES_OK;
  /* USER CODE END READ */
}

/**
  * @brief  Writes Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT USER_write (
	BYTE pdrv,          /* Physical drive nmuber to identify the drive */
	const BYTE *buff,   /* Data to be written */
	DWORD sector,       /* Sector address in LBA */
	UINT count          /* Number of sectors to write */
)
{
  /* USER CODE BEGIN WRITE */
    (void)pdrv;

    if ((Stat & STA_NOINIT) != 0u) {
        return RES_NOTRDY;
    }
    if ((Stat & STA_PROTECT) != 0u) {
        return RES_WRPRT;
    }

    DWORD addr = (CardType & CT_BLOCK) ? sector : (sector * SD_SECTOR_SIZE);

    for (UINT i = 0; i < count; i++) {
        if (sd_send_cmd(CMD24, addr) != 0x00u) {
            sd_deselect();
            return RES_ERROR;
        }
        /* 0xFE: single-block write start token. */
        if (!sd_tx_data_block(buff + i * SD_SECTOR_SIZE, 0xFEu)) {
            sd_deselect();
            return RES_ERROR;
        }
        addr += (CardType & CT_BLOCK) ? 1u : SD_SECTOR_SIZE;
    }

    sd_deselect();
    return RES_OK;
  /* USER CODE END WRITE */
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  pdrv: Physical drive number (0..)
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT USER_ioctl (
	BYTE pdrv,      /* Physical drive nmuber (0..) */
	BYTE cmd,       /* Control code */
	void *buff      /* Buffer to send/receive control data */
)
{
  /* USER CODE BEGIN IOCTL */
    (void)pdrv;

    if ((Stat & STA_NOINIT) != 0u) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        /* We never buffer -- every USER_write() already waited for the
           card to finish programming (sd_tx_data_block()'s
           sd_wait_ready()) before returning. Still must return RES_OK:
           f_sync()/f_close() treat anything else as a write failure,
           which looks exactly like "the write worked but the file is
           empty" once you check the card on a PC. sd_select() here is
           just a final not-busy check, cheap insurance. */
        if (!sd_select()) {
            return RES_ERROR;
        }
        sd_deselect();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SD_SECTOR_SIZE;
        return RES_OK;
    default:
        /* GET_SECTOR_COUNT/GET_BLOCK_SIZE (f_mkfs) and CTRL_TRIM: not
           needed -- we mount an already-formatted card, never format
           one from firmware. */
        return RES_PARERR;
    }
  /* USER CODE END IOCTL */
}
#endif /* _USE_IOCTL == 1 */

