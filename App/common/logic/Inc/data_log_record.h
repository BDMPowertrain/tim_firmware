/*
 * The Data log's file format (ADR 0021, issue 53): what the SD logger
 * writes and tools/data_log_decode.py reads. Every number is
 * little-endian. A change to a layout below bumps DATA_LOG_LAYOUT and
 * changes the decoder in the same commit.
 *
 * A file is one header, then Records back to back, nothing between them:
 *
 *   header (DATA_LOG_HEADER_SIZE): 0 magic "TDLG", 4 layout version (u16),
 *     6 image (u8, PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS), 7 flags (u8,
 *     bit 0: built from a dirty tree), 8 build ID (u32), 12 boot number
 *     (u16, the Event log's), 14 file number in the boot's directory (u16),
 *     16 uptime at open (ms, u32), 20..29 zero, 30 CRC-16 of bytes 0..29
 *
 *   Record: 0 sync DATA_LOG_SYNC (u8), 1 type (u8), 2 payload length (u16),
 *     4 uptime (ms, u32), 8 sequence number (u32), 12 payload,
 *     then CRC-16 (crc16.h) of everything before it
 *
 * Each type has one producer and its own sequence numbers (record_ring.h),
 * so a gap in one type's numbers is a Record of that type lost. A reader
 * that meets a bad CRC looks for the next sync byte that starts a whole
 * Record; a file cut off by a power loss or a pulled card ends in a torn
 * Record, which the reader reports and ignores.
 *
 * Record types and their payloads:
 *
 *   DATA_LOG_STEP  (1): the minimal step Record every image's Control step
 *     writes until its full Record exists: data_log_step_encode(). The BMS
 *     writes it for now.
 *   DATA_LOG_EVENT (2): a copy of one Event, the 32 bytes the Event log
 *     stores (event_log.h, event_log_encode()), its own CRC included.
 *   DATA_LOG_VCU_STEP (3): the VCU's full step Record, from its Control
 *     step: vcu_log_step_encode() (App/vcu/logic/Inc/vcu_log.h).
 *   DATA_LOG_IMU   (4): one accelerometer and one gyroscope sample, from
 *     the IMU task: data_log_imu_encode().
 *   DATA_LOG_DAQ_RX (5), DATA_LOG_DAQ_TX (6): one DAQ bus frame received
 *     (by canRxTask) or handed to the controller to send (by daqTask), raw:
 *     data_log_daq_encode(). The Message database decodes it (can/daq.dbc).
 *   DATA_LOG_MAILBOXES (7): what the mailboxes in a DATA_LOG_VCU_STEP are,
 *     written by the logger first in every file, sequence number 0:
 *     vcu_log_map_encode() (vcu_log.h).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef DATA_LOG_RECORD_H
#define DATA_LOG_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_frame.h"
#include "record_ring.h"

#define DATA_LOG_MAGIC       0x474C4454u /* "TDLG" */
#define DATA_LOG_LAYOUT      1u
#define DATA_LOG_HEADER_SIZE 32u
#define DATA_LOG_SYNC        0xA5u

/* A Record's bytes around its payload: 12 before, the CRC after. */
#define DATA_LOG_RECORD_OVERHEAD 14u

typedef enum {
    DATA_LOG_STEP  = 1,
    DATA_LOG_EVENT = 2,
    DATA_LOG_VCU_STEP  = 3,
    DATA_LOG_IMU       = 4,
    DATA_LOG_DAQ_RX    = 5,
    DATA_LOG_DAQ_TX    = 6,
    DATA_LOG_MAILBOXES = 7,
} data_log_type_t;

typedef struct {
    uint8_t  image; /* PARAMS_IMAGE_VCU or PARAMS_IMAGE_BMS */
    bool     dirty;
    uint32_t build_id;
    uint16_t boot;
    uint16_t file;
    uint32_t opened_ms;
} data_log_header_t;

void data_log_header_encode(uint8_t out[DATA_LOG_HEADER_SIZE], const data_log_header_t *h);

/* Bytes a Record with len payload bytes takes in the file. */
size_t data_log_record_size(uint16_t len);

/* Encodes one Record into out. Returns its size, or 0 (nothing written) if
   it does not fit in cap. */
size_t data_log_record_encode(uint8_t *out, size_t cap, const record_head_t *head,
                              const uint8_t *payload);

/* The minimal step Record. */
#define DATA_LOG_STEP_SIZE 12u

typedef struct {
    uint32_t step_count;
    uint8_t  state;
    bool     fault_active;
    bool     background_stalled;
    bool     timing_fault;
    bool     startup_ok;
    uint32_t step_us;  /* saturates at 65535 in the Record */
    uint32_t overruns; /* since power-on */
} data_log_step_t;

/* 0 step count (u32), 4 state (u8), 5 flags (u8: bit 0 fault active,
   1 background stalled, 2 timing fault, 3 start-up ok), 6 step time
   (us, u16, saturated), 8 Overruns (u32). */
void data_log_step_encode(uint8_t out[DATA_LOG_STEP_SIZE], const data_log_step_t *s);

/* The IMU Record: raw counts as the ASM330LHHG1 gives them (imu.h has
   the sensitivity the decoder scales by). */
#define DATA_LOG_IMU_SIZE 12u

typedef struct {
    int16_t accel[3]; /* x, y, z */
    int16_t gyro[3];
} data_log_imu_t;

/* 0 accel x, 2 y, 4 z, 6 gyro x, 8 y, 10 z (each i16). */
void data_log_imu_encode(uint8_t out[DATA_LOG_IMU_SIZE], const data_log_imu_t *s);

/* A DAQ frame Record: 5 bytes, then the data. */
#define DATA_LOG_DAQ_SIZE_MAX (5u + CAN_FRAME_DATA_MAX)

/* Bits on top of a CAN ID (29 bits at most) in a Record: a 29-bit ID, in
   a DAQ frame Record and in the VCU's mailbox map; FD and bit-rate
   switching, in a DAQ frame Record. */
#define DATA_LOG_ID_EXTENDED  0x80000000u
#define DATA_LOG_DAQ_FD       0x40000000u
#define DATA_LOG_DAQ_BRS      0x20000000u

/* 0 ID (u32, with the bits above), 4 length (u8), 5 the data, length
   bytes; a length over CAN_FRAME_DATA_MAX is cut to it. Returns the
   payload's size. */
uint16_t data_log_daq_encode(uint8_t out[DATA_LOG_DAQ_SIZE_MAX], const can_frame_t *f);

#endif /* DATA_LOG_RECORD_H */
