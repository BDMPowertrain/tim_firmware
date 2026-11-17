/*
 * Host tests for the Data log Record encoding
 * (App/common/logic/Inc/data_log_record.h).
 *
 * Issue 53: "File header with image, build ID, boot number and layout
 * version; each Record has a type, length, uptime, sequence number and
 * CRC-16"; "Every Event is also written as a Record carrying the same 32
 * bytes"; "Record encoding is logic; a host test checks it against the
 * Python decoder, including a sequence gap and a torn tail".
 *
 * Besides its own checks this test writes a sample file and, as literals,
 * what the Python decoder (tools/data_log_decode.py) must make of it. The
 * CTest case test_data_log_decoder then runs the decoder on the file and
 * compares (tests/host/data_log_decoder_check.cmake).
 */
#include <stdio.h>
#include <string.h>

#include "data_log_record.h"
#include "event_log.h"
#include "params.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- the encoding, byte by byte --------------------------------------- */

static void test_a_file_header_is_32_bytes_little_endian_with_its_fields_in_place(void)
{
    const data_log_header_t h = {
        .image = PARAMS_IMAGE_BMS,
        .dirty = true,
        .build_id = 0x11223344u,
        .boot = 0x0506u,
        .file = 0x0708u,
        .opened_ms = 0x0A0B0C0Du,
    };
    uint8_t b[DATA_LOG_HEADER_SIZE];
    const uint8_t want[30] = {
        'T', 'D', 'L', 'G',     /* magic */
        DATA_LOG_LAYOUT, 0x00,  /* layout version */
        0x02,                   /* image */
        0x01,                   /* flags: dirty */
        0x44, 0x33, 0x22, 0x11, /* build ID */
        0x06, 0x05,             /* boot */
        0x08, 0x07,             /* file */
        0x0D, 0x0C, 0x0B, 0x0A, /* opened, ms */
    };

    TEST_ASSERT_EQUAL_UINT32(32u, DATA_LOG_HEADER_SIZE);
    data_log_header_encode(b, &h);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, sizeof want);
}

static void test_a_record_is_sync_type_length_uptime_sequence_payload_then_a_crc(void)
{
    const record_head_t head = {
        .uptime_ms = 0x01020304u,
        .seq = 0x00000005u,
        .len = 2u,
        .type = DATA_LOG_STEP,
    };
    const uint8_t payload[2] = {0xAAu, 0xBBu};
    const uint8_t want[14] = {
        0xA5, DATA_LOG_STEP, 0x02, 0x00,
        0x04, 0x03, 0x02, 0x01,
        0x05, 0x00, 0x00, 0x00,
        0xAA, 0xBB,
    };
    uint8_t b[32];

    TEST_ASSERT_EQUAL_size_t(16u, data_log_record_encode(b, sizeof b, &head, payload));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, sizeof want);
}

static void test_a_record_that_does_not_fit_is_not_encoded(void)
{
    const record_head_t head = {.len = 4u, .type = DATA_LOG_STEP};
    const uint8_t payload[4] = {0};
    uint8_t b[17];

    TEST_ASSERT_EQUAL_size_t(0u, data_log_record_encode(b, sizeof b, &head, payload));
    TEST_ASSERT_EQUAL_size_t(18u, data_log_record_size(4u));
}

static void test_a_step_payload_holds_the_step_and_saturates_a_long_step_time(void)
{
    const data_log_step_t s = {
        .step_count = 0x01020304u,
        .state = 3u,
        .fault_active = true,
        .timing_fault = true,
        .step_us = 70000u,
        .overruns = 9u,
    };
    uint8_t b[DATA_LOG_STEP_SIZE];
    const uint8_t want[DATA_LOG_STEP_SIZE] = {
        0x04, 0x03, 0x02, 0x01, /* step count */
        0x03,                   /* state */
        0x05,                   /* fault active | timing fault */
        0xFF, 0xFF,             /* step time, saturated */
        0x09, 0x00, 0x00, 0x00, /* Overruns */
    };

    data_log_step_encode(b, &s);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, sizeof want);
}

static void test_an_event_record_carries_the_32_bytes_of_the_event_log(void)
{
    const event_t ev = {
        .boot = 42u,
        .code = 64u,
        .uptime_ms = 1012u,
        .seq = 7u,
        .data = {1u, 2u, 0u, 0u, 0x34u, 0x12u},
    };
    uint8_t b[EVENT_SIZE];
    const uint8_t want[12] = {
        42, 0, 64, 0, 0xF4, 0x03, 0, 0, 7, 0, 0, 0,
    };

    event_log_encode(&ev, b);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, sizeof want);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ev.data, &b[12], EVENT_DATA_SIZE);
}

/* ---- the sample file for the decoder ---------------------------------- */

static FILE *out;

static void put(const void *b, size_t n)
{
    TEST_ASSERT_EQUAL_size_t(n, fwrite(b, 1u, n, out));
}

static void put_step(uint32_t seq, uint32_t uptime_ms, const data_log_step_t *s,
                     size_t keep, bool corrupt)
{
    uint8_t payload[DATA_LOG_STEP_SIZE];
    uint8_t b[64];
    const record_head_t head = {
        .uptime_ms = uptime_ms,
        .seq = seq,
        .len = DATA_LOG_STEP_SIZE,
        .type = DATA_LOG_STEP,
    };

    data_log_step_encode(payload, s);
    const size_t n = data_log_record_encode(b, sizeof b, &head, payload);
    TEST_ASSERT_NOT_EQUAL(0u, n);
    if (corrupt) {
        b[14] ^= 0x10u;
    }
    put(b, keep < n ? keep : n);
}

static void write_text(const char *name, const char *text)
{
    FILE *f = fopen(name, "wb");

    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1u, strlen(text), f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
}

static void test_writes_a_sample_file_with_a_gap_a_bad_record_and_a_torn_tail(void)
{
    const data_log_header_t h = {
        .image = PARAMS_IMAGE_VCU,
        .dirty = true,
        .build_id = 0x00C0FFEEu,
        .boot = 42u,
        .file = 3u,
        .opened_ms = 1000u,
    };
    uint8_t b[64];

    out = fopen("data_log_sample.tdl", "wb");
    TEST_ASSERT_NOT_NULL(out);

    data_log_header_encode(b, &h);
    put(b, DATA_LOG_HEADER_SIZE);

    const data_log_step_t s0 = {.step_count = 0u, .state = 0u, .startup_ok = true,
                                .step_us = 250u};
    const data_log_step_t s1 = {.step_count = 1u, .state = 1u, .startup_ok = true,
                                .fault_active = true, .step_us = 300u};
    put_step(0u, 1000u, &s0, SIZE_MAX, false);
    put_step(1u, 1010u, &s1, SIZE_MAX, false);

    /* An Event copy between steps. */
    const event_t ev = {
        .boot = 42u, .code = 64u, .uptime_ms = 1012u, .seq = 7u,
        .data = {1u, 2u, 0u, 0u, 0x34u, 0x12u},
    };
    uint8_t evb[EVENT_SIZE];
    event_log_encode(&ev, evb);
    const record_head_t evh = {.uptime_ms = 1015u, .seq = 0u, .len = EVENT_SIZE,
                               .type = DATA_LOG_EVENT};
    put(b, data_log_record_encode(b, sizeof b, &evh, evb));

    /* Step 2 was dropped at the ring: a gap. */
    const data_log_step_t s3 = {.step_count = 3u, .state = 1u, .startup_ok = true,
                                .step_us = 70000u, .overruns = 1u};
    put_step(3u, 1030u, &s3, SIZE_MAX, false);
    /* Step 4 is corrupt on the card: skipped, so a second gap. */
    const data_log_step_t s4 = {.step_count = 4u, .state = 1u, .startup_ok = true};
    put_step(4u, 1040u, &s4, SIZE_MAX, true);
    const data_log_step_t s5 = {.step_count = 5u, .state = 2u, .startup_ok = true,
                                .background_stalled = true, .step_us = 12u, .overruns = 1u};
    put_step(5u, 1050u, &s5, SIZE_MAX, false);
    /* Step 6 cut off by a power loss: a torn tail of 9 bytes. */
    put_step(6u, 1060u, &s5, 9u, false);
    TEST_ASSERT_EQUAL_INT(0, fclose(out));

    /* What the decoder must make of it, written out by hand. */
    write_text("data_log_expected.step.csv",
               "seq,uptime_ms,step_count,state,fault_active,background_stalled,"
               "timing_fault,startup_ok,step_us,overruns\n"
               "0,1000,0,0,0,0,0,1,250,0\n"
               "1,1010,1,1,1,0,0,1,300,0\n"
               "3,1030,3,1,0,0,0,1,65535,1\n"
               "5,1050,5,2,0,1,0,1,12,1\n");
    write_text("data_log_expected.event.csv",
               "seq,uptime_ms,boot,event_seq,event_uptime_ms,code,data,crc_ok\n"
               "0,1015,42,7,1012,64,01020000341200000000000000000000,1\n");
    write_text("data_log_expected.summary.txt",
               "file: image VCU, build 00c0ffee dirty, boot 42, file 3, layout 1, "
               "opened at 1000 ms\n"
               "step: 4 records, 2 lost in 2 gaps\n"
               "event: 1 records, 0 lost in 0 gaps\n"
               "bad records: 1\n"
               "torn tail: 9 bytes\n");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_file_header_is_32_bytes_little_endian_with_its_fields_in_place);
    RUN_TEST(test_a_record_is_sync_type_length_uptime_sequence_payload_then_a_crc);
    RUN_TEST(test_a_record_that_does_not_fit_is_not_encoded);
    RUN_TEST(test_a_step_payload_holds_the_step_and_saturates_a_long_step_time);
    RUN_TEST(test_an_event_record_carries_the_32_bytes_of_the_event_log);
    RUN_TEST(test_writes_a_sample_file_with_a_gap_a_bad_record_and_a_torn_tail);
    return UNITY_END();
}
