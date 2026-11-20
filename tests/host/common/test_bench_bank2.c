/*
 * Writes bench_bank2.bin: bank 2's data regions as the real Parameter and
 * Event log code leaves them on the RAM-backed flash, for the bench
 * scripts' parsers (tests/bench/benchlib.py, issue 70) to read in
 * tests/host/test_benchlib.py. The file is what an OpenOCD dump of
 * 0x08040000 to 0x08052000 would hold on a board.
 *
 * What it holds, which the Python test expects:
 *   - Parameter copy A: VCU image, save counter 1, alpha = -7, beta = 900.
 *   - Parameter copy B: VCU image, save counter 2, alpha = 42, beta = 900.
 *   - Event log boot 1: 3 Events, code 1, uptimes 5, 6, 7 ms.
 *   - Event log boot 2: 70 Events, code 52, uptime 1000 + 10 n ms, data[0]
 *     = n, sequence numbers 0 to 69; 60 fit the first page, the rest open
 *     a second page (page number 2).
 */
#include <stdio.h>
#include <string.h>

#include "event_log.h"
#include "params.h"
#include "ram_flash.h"
#include "unity.h"

enum { ALPHA, BETA, TABLE_COUNT };

static const param_def_t DEFS[TABLE_COUNT] = {
    [ALPHA] = { 1u, "alpha", PARAM_I32,  10, -100,  100, false },
    [BETA]  = { 2u, "beta",  PARAM_U16, 500,    0, 1000, false },
};

static const param_table_t TABLE = { PARAMS_IMAGE_VCU, DEFS, TABLE_COUNT };

static params_t p;
static param_slot_t slots[TABLE_COUNT];
static event_log_t log_;
static uint8_t bank2[RAM_FLASH_SIZE];

void setUp(void) {}

void tearDown(void) {}

static void save(int32_t alpha, int32_t beta)
{
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, ALPHA, alpha));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BETA, beta));
    TEST_ASSERT_EQUAL(PARAMS_SAVE_OK, params_save(&p));
}

static void append(uint16_t code, uint32_t uptime_ms, uint8_t first)
{
    uint8_t data[EVENT_DATA_SIZE] = {0};

    data[0] = first;
    TEST_ASSERT_EQUAL(EVENT_APPEND_OK, event_log_append(&log_, code, uptime_ms, data));
}

static void test_write_the_sample_bank2_dump(void)
{
    ram_flash_reset();

    params_load(&p, &TABLE, ram_flash(), slots);
    save(-7, 900);
    save(42, 900);

    event_log_open(&log_, ram_flash());
    for (uint8_t n = 0; n < 3u; n++) {
        append(1u, 5u + n, n);
    }
    event_log_open(&log_, ram_flash()); /* the next start: boot 2 */
    TEST_ASSERT_EQUAL_UINT16(2u, log_.boot);
    for (uint8_t n = 0; n < 70u; n++) {
        append(52u, 1000u + 10u * n, n);
    }

    /* The load the image would do: copy B, the newer. */
    params_load(&p, &TABLE, ram_flash(), slots);
    TEST_ASSERT_EQUAL(PARAMS_LOADED, p.source);
    TEST_ASSERT_EQUAL_INT32(42, params_value(&p, ALPHA));

    ram_flash_peek(RAM_FLASH_BASE, bank2, sizeof bank2);
    FILE *f = fopen("bench_bank2.bin", "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(sizeof bank2, fwrite(bank2, 1, sizeof bank2, f));
    TEST_ASSERT_EQUAL(0, fclose(f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_the_sample_bank2_dump);
    return UNITY_END();
}
