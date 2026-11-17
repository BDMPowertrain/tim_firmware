/*
 * Host tests for the ADC window (App/common/logic/Inc/adc_window.h).
 *
 * Spec (issue 36): "TIM3 triggers the ADCs at 1 kHz into circular DMA in
 * both images; the step uses the newest samples. The first ADC conversion
 * is discarded (erratum)." Issue 55: "The control step averages the newest
 * samples at snapshot time."
 *
 * The buffer here is 4 scans of 2 channels: 8 transfers. The DMA's
 * remaining count runs 8, 7, ... 1 and reloads to 8.
 */
#include "adc_window.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define SCANS    4u
#define CHANNELS 2u

static uint16_t buf[SCANS * CHANNELS];

static void fill(uint16_t value)
{
    for (unsigned i = 0; i < SCANS * CHANNELS; i++) {
        buf[i] = value;
    }
}

static void put(unsigned scan, uint16_t ch0, uint16_t ch1)
{
    buf[scan * CHANNELS] = ch0;
    buf[scan * CHANNELS + 1u] = ch1;
}

static const adc_window_t window = { buf, SCANS, CHANNELS };

/* Scans 0 and 1 done, scan 2 not started (remaining 4 of 8): the newest
   two are 1 and 0. */
static void test_the_newest_complete_scans_are_averaged_per_channel(void)
{
    uint16_t out[CHANNELS];

    fill(ADC_WINDOW_EMPTY);
    put(0u, 100u, 2000u);
    put(1u, 110u, 2010u);

    adc_window_average(&window, 4u, 2u, out);

    TEST_ASSERT_EQUAL_UINT16(105u, out[0]);
    TEST_ASSERT_EQUAL_UINT16(2005u, out[1]);
}

/* Remaining 5 of 8: three transfers made, so the DMA is part way through
   scan 1. Scan 1 is half new, half a lap old, and is not read; the newest
   complete scans are 0 and, wrapping, 3. */
static void test_the_scan_being_written_is_skipped_and_the_window_wraps(void)
{
    uint16_t out[CHANNELS];

    put(0u, 300u, 1000u);
    put(1u, 4000u, 4000u); /* being written */
    put(2u, 9u, 9u);       /* oldest: outside a two-scan window */
    put(3u, 200u, 1100u);

    adc_window_average(&window, 5u, 2u, out);

    TEST_ASSERT_EQUAL_UINT16(250u, out[0]);
    TEST_ASSERT_EQUAL_UINT16(1050u, out[1]);
}

/* Before any scan the buffer holds only the empty marker: no sample. */
static void test_a_buffer_with_no_scans_gives_no_sample(void)
{
    uint16_t out[CHANNELS];

    fill(ADC_WINDOW_EMPTY);

    adc_window_average(&window, 8u, 3u, out);

    TEST_ASSERT_EQUAL_UINT16(ADC_WINDOW_EMPTY, out[0]);
    TEST_ASSERT_EQUAL_UINT16(ADC_WINDOW_EMPTY, out[1]);
}

/* The first scan is discarded by setting it back to the marker: a window
   over scans 0 and 1 uses scan 1 alone. */
static void test_a_discarded_scan_is_left_out_of_the_mean(void)
{
    uint16_t out[CHANNELS];

    fill(ADC_WINDOW_EMPTY);
    put(1u, 1234u, 77u);

    adc_window_average(&window, 4u, 2u, out);

    TEST_ASSERT_EQUAL_UINT16(1234u, out[0]);
    TEST_ASSERT_EQUAL_UINT16(77u, out[1]);
}

/* The mean is rounded to the nearest count, not truncated. */
static void test_the_mean_is_rounded(void)
{
    uint16_t out[CHANNELS];

    fill(ADC_WINDOW_EMPTY);
    put(0u, 100u, 100u);
    put(1u, 101u, 100u);
    put(2u, 101u, 101u);

    adc_window_average(&window, 2u, 3u, out); /* scan 3 being written */

    TEST_ASSERT_EQUAL_UINT16(101u, out[0]); /* 100.67 */
    TEST_ASSERT_EQUAL_UINT16(100u, out[1]); /* 100.33 */
}

/* A window as long as the buffer would read the scan being written: it is
   cut to scans - 1. */
static void test_the_window_never_reaches_the_scan_being_written(void)
{
    uint16_t out[CHANNELS];

    put(0u, 10u, 10u);
    put(1u, 4000u, 4000u); /* being written */
    put(2u, 20u, 20u);
    put(3u, 30u, 30u);

    adc_window_average(&window, 6u, 100u, out);

    TEST_ASSERT_EQUAL_UINT16(20u, out[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_buffer_with_no_scans_gives_no_sample);
    RUN_TEST(test_a_discarded_scan_is_left_out_of_the_mean);
    RUN_TEST(test_the_mean_is_rounded);
    RUN_TEST(test_the_window_never_reaches_the_scan_being_written);
    RUN_TEST(test_the_newest_complete_scans_are_averaged_per_channel);
    RUN_TEST(test_the_scan_being_written_is_skipped_and_the_window_wraps);
    return UNITY_END();
}
