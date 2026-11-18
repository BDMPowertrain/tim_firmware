/*
 * Host tests for the VCU's Data log Records (App/vcu/logic/Inc/vcu_log.h),
 * issue 61, ADR 0021.
 *
 * Issue 61: "Step Record at 100 Hz: each mailbox's contents with its
 * NODATA/VALID/INVALID state, pedal and brake values, state, torque
 * request, limiter and cut bits, which ceiling was lower, and the command
 * sent"; "IMU Records (accel and gyro) at 100 Hz from the IMU task".
 *
 * Expected bytes are written out by hand from the layout in vcu_log.h.
 */
#include <stdio.h>
#include <string.h>

#include "control_rx.h"
#include "data_log_record.h"
#include "unity.h"
#include "vcu_control.h"
#include "vcu_log.h"

static mailbox_t boxes[VCU_RX_COUNT];
static vcu_control_in_t in;
static vcu_control_out_t out;
static data_log_step_t common;
static uint8_t b[VCU_LOG_STEP_SIZE];

void setUp(void)
{
    memset(boxes, 0, sizeof boxes);
    memset(&out, 0, sizeof out);
    memset(&common, 0, sizeof common);
    in = (vcu_control_in_t){.mailboxes = boxes};
    memset(b, 0xEE, sizeof b);
}

void tearDown(void) {}

/* ---- the step Record ---------------------------------------------------- */

static void test_a_step_record_starts_with_the_minimal_step_payload(void)
{
    uint8_t minimal[DATA_LOG_STEP_SIZE];

    common = (data_log_step_t){.step_count = 77u, .state = 3u, .startup_ok = true,
                               .step_us = 410u, .overruns = 2u};
    data_log_step_encode(minimal, &common);
    vcu_log_step_encode(b, &common, &in, &out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(minimal, b, DATA_LOG_STEP_SIZE);
}

static void test_a_step_record_holds_the_inputs_and_what_the_step_decided(void)
{
    in.sdc_closed = true;
    in.can_running = true;
    in.rx_overflowed = true;
    out.samples_ready = true;
    out.brake_engaged = true;
    out.limiter.active = true;
    out.limiter.lower = VCU_CEILING_DISCHARGE;
    out.send_command = true;
    out.brake_light = true;
    out.pedal_check_due = true;
    in.raw = (vcu_sensor_raw_t){.apps = {0x0101, 0x0202}, .brake = {0x0303, 0x0404},
                                .supply = {0x0505, 0x0606}};
    out.sensors.apps[0] = (vcu_apps_reading_t){.travel = 1000u, .status = VCU_SENSOR_OK};
    out.sensors.apps[1] = (vcu_apps_reading_t){.travel = 0u, .status = VCU_SENSOR_OUT_OF_RANGE};
    out.sensors.brake[0] = (vcu_brake_reading_t){.pressure = 0x0123u, .status = VCU_SENSOR_OK};
    out.sensors.brake[1] = (vcu_brake_reading_t){.pressure = 0u,
                                                 .status = VCU_SENSOR_SUPPLY_FAULT};
    out.start_blockers = 0x8001u;
    out.cuts = 1u << VCU_CUT_APPS;
    out.torque_request = 1500;
    out.torque = 1200;
    out.limiter.torque_ceiling = 1200;
    out.limiter.power_ceiling_w = 72000u;
    out.limiter.trim_ppm = -150000;
    for (size_t i = 0; i < sizeof out.command; i++) {
        out.command[i] = (uint8_t)(0x10u + i);
    }

    const uint8_t want[52] = {
        0xE5,       /* 12 inputs: SDC closed, CAN running, Rx overflowed, samples ready,
                       Brake engaged */
        0xA7,       /* 13 decided: limiter active, discharge lower, command sent,
                       brake light, Pedal check due */
        0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06, /* 14 raw */
        0xE8, 0x03, 0x00, 0x00, /* 26 Pedal travel */
        VCU_SENSOR_OK, VCU_SENSOR_OUT_OF_RANGE, /* 30 */
        0x23, 0x01, 0x00, 0x00, /* 32 brake pressure */
        VCU_SENSOR_OK, VCU_SENSOR_SUPPLY_FAULT, /* 36 */
        0x01, 0x80,             /* 38 start blockers */
        0x02, 0x00,             /* 40 cuts: APPS */
        0xDC, 0x05,             /* 42 request, 150.0 N.m */
        0xB0, 0x04,             /* 44 torque sent, 120.0 N.m */
        0xB0, 0x04,             /* 46 torque ceiling */
        0x40, 0x19, 0x01, 0x00, /* 48 Power ceiling, 72000 W */
        0x10, 0xB6, 0xFD, 0xFF, /* 52 trim, -150000 ppm */
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, /* 56 the 0x0C0 sent */
    };

    vcu_log_step_encode(b, &common, &in, &out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, &b[12], sizeof want);
}

static void test_a_command_not_sent_is_logged_as_zeros(void)
{
    memset(out.command, 0x55, sizeof out.command);
    out.send_command = false;
    vcu_log_step_encode(b, &common, &in, &out);
    TEST_ASSERT_EACH_EQUAL_UINT8(0u, &b[56], 8u);
    TEST_ASSERT_EQUAL_UINT8(0u, b[13] & 0x04u);
}

static void test_each_mailbox_is_logged_with_its_state_and_the_frame_the_step_read(void)
{
    const uint8_t power[8] = {0xC3, 0x02, 0x10, 0x27, 0x20, 0x4E, 0xE8, 0x03};

    boxes[VCU_RX_BMS_POWER].state = MAILBOX_VALID;
    memcpy(boxes[VCU_RX_BMS_POWER].data, power, sizeof power);
    /* Timed out: the step reads the Substitute (all zero), not the last frame. */
    boxes[VCU_RX_INV_VOLTAGE].state = MAILBOX_INVALID;
    memset(boxes[VCU_RX_INV_VOLTAGE].data, 0x77, 8u);

    vcu_log_step_encode(b, &common, &in, &out);

    TEST_ASSERT_EQUAL_UINT8(VCU_RX_COUNT, b[64]);
    const uint8_t *row = &b[65u + 9u * VCU_RX_BMS_POWER];
    TEST_ASSERT_EQUAL_UINT8(MAILBOX_VALID, row[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(power, &row[1], 8u);
    row = &b[65u + 9u * VCU_RX_INV_VOLTAGE];
    TEST_ASSERT_EQUAL_UINT8(MAILBOX_INVALID, row[0]);
    TEST_ASSERT_EACH_EQUAL_UINT8(0u, &row[1], 8u);
    row = &b[65u + 9u * VCU_RX_BMS_STATUS];
    TEST_ASSERT_EQUAL_UINT8(MAILBOX_NODATA, row[0]);
    TEST_ASSERT_EQUAL_size_t(65u + 9u * VCU_RX_COUNT, VCU_LOG_STEP_SIZE);
}

/* ---- the mailbox map ---------------------------------------------------- */

static void test_the_map_names_each_mailbox_by_frame_id_in_step_record_order(void)
{
    uint8_t m[VCU_LOG_MAP_SIZE];

    vcu_log_map_encode(m);
    TEST_ASSERT_EQUAL_UINT8(VCU_RX_COUNT, m[0]);
    /* BMS_Status 0x080 first; BMS_Power 0x082 third. */
    const uint8_t first[4] = {0x80, 0x00, 0x00, 0x00};
    const uint8_t third[4] = {0x82, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, &m[1], 4u);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(third, &m[1u + 4u * VCU_RX_BMS_POWER], 4u);
    /* IMD_InfoGeneral 0x18FF01F4 is 29-bit: the extended bit is set. */
    const uint8_t imd[4] = {0xF4, 0x01, 0xFF, 0x98};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(imd, &m[1u + 4u * VCU_RX_IMD_INFO_GENERAL], 4u);
}

/* ---- the sample files for the decoder --------------------------------- */

/*
 * The CTest case test_data_log_vcu_decoder runs tools/data_log_decode.py on
 * the files below and checks what it makes of them
 * (tests/host/data_log_vcu_check.py, which holds the expected values), then
 * judges the two rule files' step logs with tools/power_rule_windows.py.
 */

static FILE *file;
static uint32_t seqs[8]; /* by Record type */

static void put_record(uint8_t type, uint32_t uptime_ms, const void *payload, uint16_t len)
{
    uint8_t rec[VCU_LOG_STEP_SIZE + DATA_LOG_RECORD_OVERHEAD];
    const record_head_t head = {.uptime_ms = uptime_ms, .seq = seqs[type]++, .len = len,
                                .type = type};
    const size_t n = data_log_record_encode(rec, sizeof rec, &head, (const uint8_t *)payload);

    TEST_ASSERT_NOT_EQUAL(0u, n);
    TEST_ASSERT_EQUAL_size_t(n, fwrite(rec, 1u, n, file));
}

static void open_sample(const char *name, uint32_t opened_ms)
{
    const data_log_header_t h = {
        .image = 1u, /* PARAMS_IMAGE_VCU */
        .build_id = 0x00C0FFEEu,
        .boot = 7u,
        .file = 1u,
        .opened_ms = opened_ms,
    };
    uint8_t hb[DATA_LOG_HEADER_SIZE];
    uint8_t map[VCU_LOG_MAP_SIZE];

    file = fopen(name, "wb");
    TEST_ASSERT_NOT_NULL(file);
    memset(seqs, 0, sizeof seqs);
    data_log_header_encode(hb, &h);
    TEST_ASSERT_EQUAL_size_t(sizeof hb, fwrite(hb, 1u, sizeof hb, file));
    vcu_log_map_encode(map);
    put_record(DATA_LOG_MAILBOXES, opened_ms, map, sizeof map);
}

static void put_step(uint32_t uptime_ms)
{
    vcu_log_step_encode(b, &common, &in, &out);
    put_record(DATA_LOG_VCU_STEP, uptime_ms, b, VCU_LOG_STEP_SIZE);
    common.step_count++;
}

static void set_em(float amps, float volts)
{
    boxes[VCU_RX_EM_MEASUREMENT].state = MAILBOX_VALID;
    memcpy(&boxes[VCU_RX_EM_MEASUREMENT].data[0], &amps, 4u);
    memcpy(&boxes[VCU_RX_EM_MEASUREMENT].data[4], &volts, 4u);
}

static void test_writes_a_vcu_sample_file_with_every_record_type(void)
{
    open_sample("vcu_log_sample.tdl", 2000u);

    /* Step 0: driving, BMS_Power and the Energy Meter VALID. */
    common = (data_log_step_t){.state = 3u, .startup_ok = true, .step_us = 400u};
    in.sdc_closed = true;
    in.can_running = true;
    out.limiter.active = true;
    out.limiter.lower = VCU_CEILING_DISCHARGE;
    out.limiter.power_ceiling_w = 72000u;
    out.limiter.torque_ceiling = 1200;
    out.limiter.trim_ppm = -1000;
    out.torque_request = 1500;
    out.torque = 1200;
    out.cuts = 0u;
    out.sensors.apps[0] = (vcu_apps_reading_t){.travel = 655u, .status = VCU_SENSOR_OK};
    out.sensors.apps[1] = (vcu_apps_reading_t){.travel = 650u, .status = VCU_SENSOR_OK};
    out.send_command = true;
    /* INV_Command: 120.0 N.m, forward, enabled (CM manual layout). */
    const uint8_t command[8] = {0xB0, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00};
    memcpy(out.command, command, sizeof command);
    /* BMS_Power: 100.0 A out of 500.0 V, Discharge limit 200.0 A. */
    const uint8_t power[8] = {0x00, 0x00, 0xE8, 0x03, 0x88, 0x13, 0xD0, 0x07};
    boxes[VCU_RX_BMS_POWER].state = MAILBOX_VALID;
    memcpy(boxes[VCU_RX_BMS_POWER].data, power, sizeof power);
    set_em(120.5f, 600.0f);
    put_step(2000u);

    /* Step 1 was dropped at the ring: a gap. */
    seqs[DATA_LOG_VCU_STEP]++;
    common.step_count++;

    /* Step 2: every mailbox timed out, an APPS cut, nothing sent. */
    for (size_t i = 0; i < VCU_RX_COUNT; i++) {
        boxes[i].state = MAILBOX_INVALID;
    }
    out = (vcu_control_out_t){.cuts = 1u << VCU_CUT_APPS, .torque_request = 0};
    out.sensors.apps[1].status = VCU_SENSOR_OUT_OF_RANGE;
    put_step(2020u);

    /* Two IMU Records: 1 g down, then a 70 dps yaw. */
    const data_log_imu_t still = {.accel = {0, 0, 4098}, .gyro = {0, 0, 0}};
    const data_log_imu_t yaw = {.accel = {0, 0, 4098}, .gyro = {0, 0, 1000}};
    uint8_t imu[DATA_LOG_IMU_SIZE];
    data_log_imu_encode(imu, &still);
    put_record(DATA_LOG_IMU, 2001u, imu, sizeof imu);
    data_log_imu_encode(imu, &yaw);
    put_record(DATA_LOG_IMU, 2011u, imu, sizeof imu);

    /* DAQ bus frames: a BMS status heard, an unknown frame heard, ours sent. */
    uint8_t daq[DATA_LOG_DAQ_SIZE_MAX];
    const can_frame_t bms = {.id = 0x280u, .len = 8u, .is_fd = true, .is_brs = true,
                             .data = {0x11, 0x00, 0, 0, 0x05, 0, 0, 0}};
    put_record(DATA_LOG_DAQ_RX, 2005u, daq, data_log_daq_encode(daq, &bms));
    const can_frame_t odd = {.id = 0x1ABCDEFu, .len = 3u, .is_extended = true,
                             .data = {1, 2, 3}};
    put_record(DATA_LOG_DAQ_RX, 2006u, daq, data_log_daq_encode(daq, &odd));
    const can_frame_t ours = {.id = 0x290u, .len = 8u, .is_fd = true, .is_brs = true,
                              .data = {0x01, 0, 0, 0, 0x02, 0, 0, 0}};
    put_record(DATA_LOG_DAQ_TX, 2007u, daq, data_log_daq_encode(daq, &ours));

    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* A second of steps with the Energy Meter at 600 V; the current at amps,
   or at high_amps from 300 ms for 150 ms. Step 50's meter is not VALID: a
   row with no power, which the rule check skips. */
static void write_rule_file(const char *name, float amps, float high_amps)
{
    open_sample(name, 5000u);
    common = (data_log_step_t){.state = 3u, .startup_ok = true};
    for (uint32_t i = 0; i < 100u; i++) {
        const uint32_t t = 5000u + 10u * i;
        set_em((i >= 30u && i < 45u) ? high_amps : amps, 600.0f);
        if (i == 50u) {
            boxes[VCU_RX_EM_MEASUREMENT].state = MAILBOX_INVALID;
        }
        put_step(t);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* A file whose map Record was lost (say a bad CRC): its steps are still
   decoded, the mailboxes named by position and left raw. */
static void test_writes_a_step_log_with_no_mailbox_map(void)
{
    const data_log_header_t h = {.image = 1u, .build_id = 1u, .boot = 7u, .file = 2u,
                                 .opened_ms = 9000u};
    uint8_t hb[DATA_LOG_HEADER_SIZE];

    file = fopen("vcu_log_no_map.tdl", "wb");
    TEST_ASSERT_NOT_NULL(file);
    memset(seqs, 0, sizeof seqs);
    data_log_header_encode(hb, &h);
    TEST_ASSERT_EQUAL_size_t(sizeof hb, fwrite(hb, 1u, sizeof hb, file));
    memset(boxes, 0, sizeof boxes);
    boxes[VCU_RX_BMS_POWER].state = MAILBOX_VALID;
    boxes[VCU_RX_BMS_POWER].data[2] = 0xE8;
    common = (data_log_step_t){.state = 1u};
    put_step(9000u);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_writes_two_step_logs_for_the_rule_window_check(void)
{
    /* 72 kW throughout: no Violation. */
    write_rule_file("vcu_rule_pass.tdl", 120.0f, 120.0f);
    /* 90 kW for 150 ms: above 80 kW continuously for 100 ms (EV.3.4.1). */
    write_rule_file("vcu_rule_fail.tdl", 120.0f, 150.0f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_step_record_starts_with_the_minimal_step_payload);
    RUN_TEST(test_a_step_record_holds_the_inputs_and_what_the_step_decided);
    RUN_TEST(test_a_command_not_sent_is_logged_as_zeros);
    RUN_TEST(test_each_mailbox_is_logged_with_its_state_and_the_frame_the_step_read);
    RUN_TEST(test_the_map_names_each_mailbox_by_frame_id_in_step_record_order);
    RUN_TEST(test_writes_a_vcu_sample_file_with_every_record_type);
    RUN_TEST(test_writes_a_step_log_with_no_mailbox_map);
    RUN_TEST(test_writes_two_step_logs_for_the_rule_window_check);
    return UNITY_END();
}
