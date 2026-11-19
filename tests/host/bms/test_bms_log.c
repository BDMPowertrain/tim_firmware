/*
 * Host tests for the BMS's Data log Records (App/bms/logic/Inc/bms_log.h),
 * issue 69, ADR 0021.
 *
 * Issue 69: "Pack-level Record every sweep (50 Hz); full snapshot Record
 * every fifth sweep (10 Hz) with 128 voltages, 128 temperatures and charge
 * and balancing status"; "The Python decoder handles both BMS Record
 * types".
 *
 * Expected bytes are written out by hand from the layout in bms_log.h.
 */
#include <stdio.h>
#include <string.h>

#include "bms_control.h"
#include "bms_log.h"
#include "data_log_record.h"
#include "unity.h"

static bq_sweep_t sweep;
static mailbox_t charger;
static bms_control_in_t in;
static bms_control_out_t out;
static data_log_step_t common;
static uint8_t b[BMS_LOG_SNAPSHOT_SIZE];

void setUp(void)
{
    memset(&sweep, 0, sizeof sweep);
    memset(&charger, 0, sizeof charger);
    memset(&out, 0, sizeof out);
    memset(&common, 0, sizeof common);
    in = (bms_control_in_t){.sweep = &sweep, .charger = &charger};
    memset(b, 0xEE, sizeof b);
}

void tearDown(void) {}

static uint16_t u16_at(size_t at)
{
    return (uint16_t)(b[at] | (b[at + 1u] << 8));
}

/* ---- the pack Record ---------------------------------------------------- */

static void test_a_pack_record_starts_with_the_minimal_step_payload(void)
{
    uint8_t minimal[DATA_LOG_STEP_SIZE];

    common = (data_log_step_t){.step_count = 77u, .state = 2u, .startup_ok = true,
                               .step_us = 9100u, .overruns = 1u};
    data_log_step_encode(minimal, &common);
    bms_log_pack_encode(b, &common, BQ_CHAIN_RUNNING, &in, &out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(minimal, b, DATA_LOG_STEP_SIZE);
}

static void test_a_pack_record_holds_what_the_step_saw_and_decided(void)
{
    in.swept = true;
    in.sdc_closed = true;
    in.bms_ok_fb = true;
    in.error_passive = true;
    in.pack_current = (bms_current_t){.deci_a = -1234, .valid = true};
    charger.state = MAILBOX_VALID;
    sweep.devices_read = 0xBFFFu; /* device 15 not read */
    sweep.bridge_read = true;
    sweep.bridge_fault = 0x21u;
    out.bms_ok = true;
    out.open_wire_due = true;
    out.active = 0x0011u;
    out.seen = 0x0113u;
    out.charge = BMS_CHARGE_CHARGING;
    out.balance_state = 2u;
    out.balance_reason = 5u;
    out.cell_spread_mv = 42u;
    out.discharge_limit_da = 0u;
    out.charge_limit_da = 250u;
    out.summary = (bms_pack_summary_t){
        .cells = 120u, .sum_mv = 446400u, .min_mv = 3650u, .max_mv = 3790u,
        .min_cell = 17u, .max_cell = 100u, .temps = 119u, .sum_dc = 29750,
        .min_dc = -15, .max_dc = 512, .min_temp = 3u, .max_temp = 127u,
    };

    bms_log_pack_encode(b, &common, BQ_CHAIN_RUNNING, &in, &out);

    /* inputs: swept 0x01 | sdc closed 0x02 | FB.BMS_OK 0x08 | error
       passive 0x40 = 0x4B. decided: BMS_OK 0x01 | open-wire due 0x04 |
       charger VALID 0x08 | current valid 0x10 | bridge read 0x20 = 0x3D. */
    TEST_ASSERT_EQUAL_HEX8(0x4Bu, b[12]);
    TEST_ASSERT_EQUAL_HEX8(0x3Du, b[13]);
    TEST_ASSERT_EQUAL_HEX16(0x0011u, u16_at(14));
    TEST_ASSERT_EQUAL_HEX16(0x0113u, u16_at(16));
    TEST_ASSERT_EQUAL_UINT8(BQ_CHAIN_RUNNING, b[18]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CHARGING, b[19]);
    TEST_ASSERT_EQUAL_UINT8(2u, b[20]);
    TEST_ASSERT_EQUAL_UINT8(5u, b[21]);
    TEST_ASSERT_EQUAL_HEX16(0xBFFFu, u16_at(22));
    TEST_ASSERT_EQUAL_HEX8(0x21u, b[24]);
    TEST_ASSERT_EQUAL_UINT8(120u, b[25]);
    TEST_ASSERT_EQUAL_UINT8(119u, b[26]);
    TEST_ASSERT_EQUAL_UINT8(0u, b[27]);
    /* 446400 = 0x0006CFC0 */
    const uint8_t sum[4] = {0xC0u, 0xCFu, 0x06u, 0x00u};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sum, &b[28], 4u);
    TEST_ASSERT_EQUAL_UINT16(3650u, u16_at(32));
    TEST_ASSERT_EQUAL_UINT16(3790u, u16_at(34));
    TEST_ASSERT_EQUAL_UINT8(17u, b[36]);
    TEST_ASSERT_EQUAL_UINT8(100u, b[37]);
    TEST_ASSERT_EQUAL_HEX16(0xFFF1u, u16_at(38)); /* -15 */
    TEST_ASSERT_EQUAL_UINT16(512u, u16_at(40));
    TEST_ASSERT_EQUAL_UINT8(3u, b[42]);
    TEST_ASSERT_EQUAL_UINT8(127u, b[43]);
    TEST_ASSERT_EQUAL_HEX16(0xFB2Eu, u16_at(44)); /* -1234 */
    TEST_ASSERT_EQUAL_UINT16(0u, u16_at(46));
    TEST_ASSERT_EQUAL_UINT16(250u, u16_at(48));
    TEST_ASSERT_EQUAL_UINT16(42u, u16_at(50));
    /* Nothing past the pack Record's end. */
    TEST_ASSERT_EQUAL_HEX8(0xEEu, b[BMS_LOG_PACK_SIZE]);
}

static void test_the_remaining_input_bits_each_have_their_place(void)
{
    in.nfault_ok = true;
    in.can_running = true;
    in.bus_off_latched = true;
    in.rx_overflowed = true;
    out.under_low_voltage_limit = true;
    out.balance_command.op = BQ_BALANCE_SET;

    bms_log_pack_encode(b, &common, BQ_CHAIN_WAKE, &in, &out);

    /* NFAULT 0x04 | CAN1 running 0x10 | bus-off latch 0x20 | RX overflow
       0x80; under the limit 0x02 | balancing command 0x40. */
    TEST_ASSERT_EQUAL_HEX8(0xB4u, b[12]);
    TEST_ASSERT_EQUAL_HEX8(0x42u, b[13]);
}

/* ---- the snapshot Record ------------------------------------------------ */

static void test_a_snapshot_is_the_pack_record_then_every_cell_sensor_and_bleed_flag(void)
{
    uint8_t pack[BMS_LOG_PACK_SIZE];

    common.step_count = 5u;
    in.swept = true;
    sweep.devices_read = BQ_SWEEP_ALL_DEVICES;
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = (uint16_t)(3000u + i);
        sweep.temp_mv[i] = (uint16_t)(1500u + i);
    }
    out.bleeding[0] = 0x05u;
    out.bleeding[15] = 0xA0u;

    bms_log_pack_encode(pack, &common, BQ_CHAIN_RUNNING, &in, &out);
    bms_log_snapshot_encode(b, &common, BQ_CHAIN_RUNNING, &in, &out);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(pack, b, BMS_LOG_PACK_SIZE);
    TEST_ASSERT_EQUAL_UINT16(3000u, u16_at(52));         /* module 0 cell 0 */
    TEST_ASSERT_EQUAL_UINT16(3127u, u16_at(52 + 2 * 127)); /* module 15 cell 7 */
    TEST_ASSERT_EQUAL_UINT16(1500u, u16_at(308));        /* module 0 sensor 0 */
    TEST_ASSERT_EQUAL_UINT16(1627u, u16_at(308 + 2 * 127));
    TEST_ASSERT_EQUAL_HEX8(0x05u, b[564]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, b[565]);
    TEST_ASSERT_EQUAL_HEX8(0xA0u, b[579]);
    TEST_ASSERT_EQUAL_size_t(580u, BMS_LOG_SNAPSHOT_SIZE);
}

static void test_a_snapshot_is_due_on_every_fifth_sweep_only(void)
{
    TEST_ASSERT_FALSE(bms_log_snapshot_due(true, 4u));
    TEST_ASSERT_TRUE(bms_log_snapshot_due(true, 5u));
    TEST_ASSERT_FALSE(bms_log_snapshot_due(true, 6u));
    TEST_ASSERT_TRUE(bms_log_snapshot_due(true, 10u));
    /* A step with no Sweep has nothing to snapshot, whatever the count. */
    TEST_ASSERT_FALSE(bms_log_snapshot_due(false, 10u));
    TEST_ASSERT_FALSE(bms_log_snapshot_due(false, 0u));
}

/* ---- a sample file for the decoder --------------------------------------
 * The host test test_data_log_bms_decoder runs tools/data_log_decode.py on
 * this file and checks what it makes of it
 * (tests/host/data_log_bms_check.py, which holds the expected values).
 * Each type has its own ring and sequence numbers: here pack 0-4, 5 lost,
 * 6, and snapshot 0 on pack 4's step. */

static FILE *file;
static uint32_t seqs[16]; /* by Record type */

static void put_record(uint8_t type, uint32_t uptime_ms, const void *payload, uint16_t len)
{
    uint8_t rec[BMS_LOG_SNAPSHOT_SIZE + DATA_LOG_RECORD_OVERHEAD];
    const record_head_t head = {.uptime_ms = uptime_ms, .seq = seqs[type]++, .len = len,
                                .type = type};
    const size_t n = data_log_record_encode(rec, sizeof rec, &head, (const uint8_t *)payload);

    TEST_ASSERT_NOT_EQUAL(0u, n);
    TEST_ASSERT_EQUAL_size_t(n, fwrite(rec, 1u, n, file));
}

static void put_step(uint32_t uptime_ms, bool snapshot)
{
    if (snapshot) {
        bms_log_snapshot_encode(b, &common, BQ_CHAIN_RUNNING, &in, &out);
        put_record(DATA_LOG_BMS_SNAPSHOT, uptime_ms, b, BMS_LOG_SNAPSHOT_SIZE);
    }
    bms_log_pack_encode(b, &common, BQ_CHAIN_RUNNING, &in, &out);
    put_record(DATA_LOG_BMS_PACK, uptime_ms, b, BMS_LOG_PACK_SIZE);
    common.step_count++;
}

static void test_writes_a_bms_sample_file_with_both_record_types(void)
{
    const data_log_header_t h = {
        .image = 2u, /* PARAMS_IMAGE_BMS */
        .build_id = 0x0000B115u,
        .boot = 9u,
        .file = 1u,
        .opened_ms = 4000u,
    };
    uint8_t hb[DATA_LOG_HEADER_SIZE];

    file = fopen("bms_log_sample.tdl", "wb");
    TEST_ASSERT_NOT_NULL(file);
    memset(seqs, 0, sizeof seqs);
    data_log_header_encode(hb, &h);
    TEST_ASSERT_EQUAL_size_t(sizeof hb, fwrite(hb, 1u, sizeof hb, file));

    /* Charging, every device read, every cell 3.900 V but module 2's
       cell 5 at 3.950 V, every sensor 1860 mV (25.0 C) but module 7's
       sensor 1 open (3000 mV); cells 0 and 2 of module 0 bleeding. */
    common = (data_log_step_t){.step_count = 100u, .state = 1u, .startup_ok = true,
                               .step_us = 9000u};
    in.swept = true;
    in.sdc_closed = true;
    in.nfault_ok = true;
    in.bms_ok_fb = true;
    in.can_running = true;
    charger.state = MAILBOX_VALID;
    sweep.devices_read = BQ_SWEEP_ALL_DEVICES;
    sweep.bridge_read = true;
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = 3900u;
        sweep.temp_mv[i] = 1860u;
    }
    sweep.cell_mv[2 * 8 + 5] = 3950u;
    sweep.temp_mv[7 * 8 + 1] = 3000u;
    out.bms_ok = true;
    out.charge = BMS_CHARGE_CHARGING;
    out.balance_state = 1u;
    out.cell_spread_mv = 50u;
    out.charge_limit_da = 125u;
    out.bleeding[0] = 0x05u;
    out.summary = (bms_pack_summary_t){
        .cells = 128u, .sum_mv = 499250u, .min_mv = 3900u, .max_mv = 3950u,
        .min_cell = 0u, .max_cell = 21u, .temps = 127u, .sum_dc = 31750,
        .min_dc = 250, .max_dc = 250, .min_temp = 0u, .max_temp = 0u,
    };

    put_step(4000u, false);
    put_step(4020u, false);
    put_step(4040u, false);
    put_step(4060u, false);
    put_step(4080u, true);
    seqs[DATA_LOG_BMS_PACK]++; /* one pack Record lost at its ring */
    common.step_count++;
    in.pack_current = (bms_current_t){.deci_a = -125, .valid = true};
    put_step(4120u, false);

    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_pack_record_starts_with_the_minimal_step_payload);
    RUN_TEST(test_a_pack_record_holds_what_the_step_saw_and_decided);
    RUN_TEST(test_the_remaining_input_bits_each_have_their_place);
    RUN_TEST(test_a_snapshot_is_the_pack_record_then_every_cell_sensor_and_bleed_flag);
    RUN_TEST(test_a_snapshot_is_due_on_every_fifth_sweep_only);
    RUN_TEST(test_writes_a_bms_sample_file_with_both_record_types);
    return UNITY_END();
}
