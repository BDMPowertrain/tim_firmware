/*
 * Host tests for Parameter storage (App/common/logic/Inc/params.h,
 * ADR 0014 and 0015), through load, set and save on the RAM-backed flash
 * adapter. A "reset" is a fresh params_load() on the same flash.
 */
#include <string.h>

#include "params.h"
#include "ram_flash.h"
#include "unity.h"

enum { ALPHA, BETA, FLAG, TABLE_COUNT };

static const param_def_t DEFS[TABLE_COUNT] = {
    [ALPHA] = { 1u, "alpha", PARAM_I32,  10, -100,  100, false },
    [BETA]  = { 2u, "beta",  PARAM_U16, 500,    0, 1000, false },
    [FLAG]  = { 3u, "flag",  PARAM_BOOL,  0,    0,    1, false },
};

static const param_table_t TABLE = { PARAMS_IMAGE_VCU, DEFS, TABLE_COUNT };

static params_t p;
static param_slot_t slots[TABLE_COUNT];

void setUp(void)
{
    ram_flash_reset();
}

void tearDown(void) {}

/* A reset: power back, then the start-up load. */
static void reset_with(const param_table_t *table, param_slot_t *s)
{
    ram_flash_power_on();
    params_load(&p, table, ram_flash(), s);
}

static void reset(void)
{
    reset_with(&TABLE, slots);
}

static void assert_values(int32_t alpha, int32_t beta, int32_t flag)
{
    TEST_ASSERT_EQUAL_INT32(alpha, params_value(&p, ALPHA));
    TEST_ASSERT_EQUAL_INT32(beta, params_value(&p, BETA));
    TEST_ASSERT_EQUAL_INT32(flag, params_value(&p, FLAG));
}

/* Sets all three and saves; the save must succeed. */
static void save_values(int32_t alpha, int32_t beta, int32_t flag)
{
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, ALPHA, alpha));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BETA, beta));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, FLAG, flag));
    TEST_ASSERT_EQUAL(PARAMS_SAVE_OK, params_save(&p));
}

static void test_blank_flash_runs_on_defaults(void)
{
    reset();

    TEST_ASSERT_EQUAL(PARAMS_DEFAULTED, p.source);
    TEST_ASSERT_EQUAL(PARAMS_COPY_EMPTY, p.copy_status[0]);
    TEST_ASSERT_EQUAL(PARAMS_COPY_EMPTY, p.copy_status[1]);
    assert_values(10, 500, 0);
    TEST_ASSERT_EQUAL(PARAM_DEFAULT_NO_COPY, slots[ALPHA].origin);
}

static void test_a_saved_set_is_in_force_after_a_reset_and_not_before(void)
{
    reset();
    save_values(-7, 900, 1);

    assert_values(10, 500, 0);

    reset();
    TEST_ASSERT_EQUAL(PARAMS_LOADED, p.source);
    assert_values(-7, 900, 1);
    TEST_ASSERT_EQUAL(PARAM_FROM_COPY, slots[BETA].origin);
}

static void test_a_save_overwrites_the_older_copy_only(void)
{
    uint8_t before[PARAMS_COPY_SIZE];
    uint8_t after[PARAMS_COPY_SIZE];

    reset();
    save_values(1, 1, 0);   /* counter 1, copy A */
    save_values(2, 2, 0);   /* counter 2, copy B */
    reset();
    TEST_ASSERT_EQUAL(PARAMS_COPY_GOOD, p.copy_status[0]);
    TEST_ASSERT_EQUAL(PARAMS_COPY_GOOD, p.copy_status[1]);
    TEST_ASSERT_EQUAL_UINT32(1u, p.copy_counter[0]);
    TEST_ASSERT_EQUAL_UINT32(2u, p.copy_counter[1]);
    ram_flash_peek(PARAMS_COPY_B_ADDR, before, sizeof before);

    save_values(3, 3, 1);   /* counter 3, over A */

    ram_flash_peek(PARAMS_COPY_B_ADDR, after, sizeof after);
    TEST_ASSERT_EQUAL_MEMORY(before, after, sizeof before);
    reset();
    TEST_ASSERT_EQUAL_UINT8(0u, p.copy_loaded);
    TEST_ASSERT_EQUAL_UINT32(3u, p.copy_counter[0]);
    assert_values(3, 3, 1);
}

/* Power is lost at every erase and program of a save, in turn. After each
   cut the previous set is in force, and the next save works. */
static void test_a_save_cut_anywhere_leaves_the_previous_set(void)
{
    reset();
    save_values(1, 100, 0);
    save_values(2, 200, 1);
    uint32_t before = ram_flash_ops();
    save_values(3, 300, 0);
    uint32_t opsPerSave = ram_flash_ops() - before;

    TEST_ASSERT_GREATER_THAN_UINT32(3u, opsPerSave);

    for (uint32_t cut = 0; cut < opsPerSave; cut++) {
        ram_flash_reset();
        reset();
        save_values(1, 100, 0);
        save_values(2, 200, 1);

        (void)params_set(&p, ALPHA, 3);
        (void)params_set(&p, BETA, 300);
        ram_flash_cut_after(cut);
        TEST_ASSERT_NOT_EQUAL(PARAMS_SAVE_OK, params_save(&p));

        reset();
        TEST_ASSERT_EQUAL_MESSAGE(PARAMS_LOADED, p.source, "previous set lost");
        assert_values(2, 200, 1);

        save_values(4, 400, 0);
        reset();
        assert_values(4, 400, 0);
    }
}

/* The last value byte of the first pair: covered by the CRC only. */
#define FIRST_VALUE_BYTE (PARAMS_HEADER_SIZE + 7u)

static void test_a_corrupt_newest_copy_falls_back_to_the_older(void)
{
    reset();
    save_values(1, 100, 0);   /* A */
    save_values(2, 200, 1);   /* B, newest */
    ram_flash_flip(PARAMS_COPY_B_ADDR + FIRST_VALUE_BYTE, 0x01u);

    reset();

    TEST_ASSERT_EQUAL(PARAMS_LOADED, p.source);
    TEST_ASSERT_EQUAL(PARAMS_COPY_BAD_CRC, p.copy_status[1]);
    TEST_ASSERT_EQUAL_UINT8(0u, p.copy_loaded);
    assert_values(1, 100, 0);
}

/* The newest copy checks good, then its first pair faults when it is read
   again for the values: the older copy is loaded instead. */
static void test_a_read_fault_while_loading_falls_back_to_the_older(void)
{
    reset();
    save_values(1, 100, 0);   /* A */
    save_values(2, 200, 1);   /* B, newest */
    ram_flash_fail_read(PARAMS_COPY_B_ADDR + PARAMS_HEADER_SIZE, 2u);

    reset();

    TEST_ASSERT_EQUAL(PARAMS_LOADED, p.source);
    TEST_ASSERT_EQUAL(PARAMS_COPY_READ_FAULT, p.copy_status[1]);
    TEST_ASSERT_EQUAL_UINT8(0u, p.copy_loaded);
    assert_values(1, 100, 0);
    TEST_ASSERT_FALSE(params_defaulted(&p));
}

static void test_both_copies_corrupt_runs_on_defaults(void)
{
    reset();
    save_values(1, 100, 0);
    save_values(2, 200, 1);
    ram_flash_flip(PARAMS_COPY_A_ADDR + FIRST_VALUE_BYTE, 0x80u);
    ram_flash_flip(PARAMS_COPY_B_ADDR, 0x01u); /* the marker */

    reset();

    TEST_ASSERT_EQUAL(PARAMS_DEFAULTED, p.source);
    TEST_ASSERT_EQUAL(PARAMS_COPY_BAD_CRC, p.copy_status[0]);
    TEST_ASSERT_EQUAL(PARAMS_COPY_BAD_HEADER, p.copy_status[1]);
    assert_values(10, 500, 0);
}

static void test_a_save_after_a_corrupt_copy_replaces_the_corrupt_one(void)
{
    reset();
    save_values(1, 100, 0);   /* A, counter 1 */
    save_values(2, 200, 1);   /* B, counter 2 */
    ram_flash_flip(PARAMS_COPY_B_ADDR + FIRST_VALUE_BYTE, 0x01u);
    reset();                  /* runs on A */

    save_values(3, 300, 0);

    reset();
    TEST_ASSERT_EQUAL(PARAMS_COPY_GOOD, p.copy_status[0]);
    TEST_ASSERT_EQUAL_UINT8(1u, p.copy_loaded);
    TEST_ASSERT_EQUAL_UINT32(2u, p.copy_counter[1]);
    assert_values(3, 300, 0);
}

static const param_table_t BMS_TABLE = { PARAMS_IMAGE_BMS, DEFS, TABLE_COUNT };

static void test_the_other_images_copy_is_not_loaded(void)
{
    param_slot_t bmsSlots[TABLE_COUNT];

    reset_with(&BMS_TABLE, bmsSlots);
    save_values(1, 100, 0);

    reset();

    TEST_ASSERT_EQUAL(PARAMS_DEFAULTED, p.source);
    TEST_ASSERT_EQUAL(PARAMS_COPY_WRONG_IMAGE, p.copy_status[0]);
    assert_values(10, 500, 0);
}

/* To the other image both our copies are bad, so its save takes A. Ours in
   B is still the one loaded. */
static void test_our_copy_is_loaded_beside_the_other_images(void)
{
    param_slot_t bmsSlots[TABLE_COUNT];

    reset();
    save_values(1, 100, 0);   /* ours, A */
    save_values(2, 200, 1);   /* ours, B */
    reset_with(&BMS_TABLE, bmsSlots);
    save_values(3, 300, 0);   /* theirs, over A */

    reset();

    TEST_ASSERT_EQUAL(PARAMS_COPY_WRONG_IMAGE, p.copy_status[0]);
    TEST_ASSERT_EQUAL(PARAMS_LOADED, p.source);
    TEST_ASSERT_EQUAL_UINT8(1u, p.copy_loaded);
    assert_values(2, 200, 1);
}

/* What an older or newer image stored: alpha with wider bounds, beta, and
   an ID this image does not know. No flag. */
enum { OLD_ALPHA, OLD_BETA, OLD_GONE, OLD_COUNT };

static const param_def_t OLD_DEFS[OLD_COUNT] = {
    [OLD_ALPHA] = { 1u, "alpha", PARAM_I32,  0, -1000, 1000, false },
    [OLD_BETA]  = { 2u, "beta",  PARAM_U16,  0,     0, 1000, false },
    [OLD_GONE]  = { 9u, "gone",  PARAM_U8,   0,     0,  255, false },
};

static const param_table_t OLD_TABLE = { PARAMS_IMAGE_VCU, OLD_DEFS, OLD_COUNT };

static void test_unknown_ignored_missing_defaulted_out_of_range_defaulted_and_reported(void)
{
    param_slot_t oldSlots[OLD_COUNT];

    reset_with(&OLD_TABLE, oldSlots);
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, OLD_ALPHA, 500));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, OLD_BETA, 42));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, OLD_GONE, 7));
    TEST_ASSERT_EQUAL(PARAMS_SAVE_OK, params_save(&p));

    reset();

    TEST_ASSERT_EQUAL(PARAMS_LOADED, p.source);
    assert_values(10, 42, 0);
    TEST_ASSERT_EQUAL(PARAM_DEFAULT_OUT_OF_RANGE, slots[ALPHA].origin);
    TEST_ASSERT_EQUAL_INT32(500, slots[ALPHA].rejected);
    TEST_ASSERT_EQUAL(PARAM_FROM_COPY, slots[BETA].origin);
    TEST_ASSERT_EQUAL(PARAM_DEFAULT_MISSING, slots[FLAG].origin);
    TEST_ASSERT_EQUAL_UINT16(1u, p.unknown);
    TEST_ASSERT_EQUAL_UINT16(1u, p.missing);
    TEST_ASSERT_EQUAL_UINT16(1u, p.out_of_range);
    TEST_ASSERT_TRUE(params_defaulted(&p));
}

/* "Parameters defaulted" (ADR 0015): no valid copy, or any value missing or
   out of range. */
static void test_parameters_defaulted_only_when_a_value_is_a_default(void)
{
    reset();
    TEST_ASSERT_TRUE(params_defaulted(&p));

    save_values(1, 100, 0);
    reset();
    TEST_ASSERT_FALSE(params_defaulted(&p));

    params_load(&p, &TABLE, NULL, slots);
    TEST_ASSERT_TRUE(params_defaulted(&p));
}

static void test_without_flash_defaults_and_refuses_every_save(void)
{
    params_load(&p, &TABLE, NULL, slots);

    TEST_ASSERT_EQUAL(PARAMS_NO_FLASH, p.source);
    assert_values(10, 500, 0);
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, ALPHA, 3));
    TEST_ASSERT_EQUAL(PARAMS_SAVE_NO_FLASH, params_save(&p));
    TEST_ASSERT_EQUAL_UINT32(0u, ram_flash_ops());
}

static void test_a_set_outside_the_bounds_is_refused(void)
{
    reset();

    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, ALPHA, 101));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, ALPHA, -101));
    TEST_ASSERT_EQUAL(PARAMS_SET_NO_SUCH_INDEX, params_set(&p, TABLE_COUNT, 0));
    TEST_ASSERT_EQUAL_INT32(10, slots[ALPHA].pending);
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, ALPHA, 100));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, ALPHA, -100));
}

static void test_a_parameter_is_found_by_name(void)
{
    reset();

    TEST_ASSERT_EQUAL(BETA, params_find(&p, "beta"));
    TEST_ASSERT_EQUAL(TABLE_COUNT, params_find(&p, "gamma"));
}

static void test_a_bad_table_is_refused(void)
{
    static const param_def_t dupId[] = {
        { 1u, "a", PARAM_U8, 0, 0, 1, false },
        { 1u, "b", PARAM_U8, 0, 0, 1, false },
    };
    static const param_def_t defaultOutside[] = { { 1u, "a", PARAM_U8, 5, 0, 4, false } };
    static const param_def_t outsideType[] = { { 1u, "a", PARAM_U8, 0, 0, 256, false } };
    static const param_def_t boolTooWide[] = { { 1u, "a", PARAM_BOOL, 0, 0, 2, false } };
    const param_table_t bad[] = {
        { PARAMS_IMAGE_VCU, dupId, 2u },
        { PARAMS_IMAGE_VCU, defaultOutside, 1u },
        { PARAMS_IMAGE_VCU, outsideType, 1u },
        { PARAMS_IMAGE_VCU, boolTooWide, 1u },
        { PARAMS_IMAGE_VCU, DEFS, PARAMS_MAX_COUNT + 1u },
    };

    TEST_ASSERT_TRUE(params_table_ok(&TABLE));
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(params_table_ok(&bad[i]), "bad table accepted");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blank_flash_runs_on_defaults);
    RUN_TEST(test_a_saved_set_is_in_force_after_a_reset_and_not_before);
    RUN_TEST(test_a_save_overwrites_the_older_copy_only);
    RUN_TEST(test_a_save_cut_anywhere_leaves_the_previous_set);
    RUN_TEST(test_a_corrupt_newest_copy_falls_back_to_the_older);
    RUN_TEST(test_a_read_fault_while_loading_falls_back_to_the_older);
    RUN_TEST(test_both_copies_corrupt_runs_on_defaults);
    RUN_TEST(test_a_save_after_a_corrupt_copy_replaces_the_corrupt_one);
    RUN_TEST(test_the_other_images_copy_is_not_loaded);
    RUN_TEST(test_our_copy_is_loaded_beside_the_other_images);
    RUN_TEST(test_unknown_ignored_missing_defaulted_out_of_range_defaulted_and_reported);
    RUN_TEST(test_parameters_defaulted_only_when_a_value_is_a_default);
    RUN_TEST(test_without_flash_defaults_and_refuses_every_save);
    RUN_TEST(test_a_set_outside_the_bounds_is_refused);
    RUN_TEST(test_a_parameter_is_found_by_name);
    RUN_TEST(test_a_bad_table_is_refused);
    return UNITY_END();
}
