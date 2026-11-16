/*
 * Host tests for the Mailbox (App/common/logic/Inc/mailbox.h, ADR 0004).
 *
 * Two test frames stand in for the two kinds of received frame: PLAIN is a
 * third-party frame (length check only) and OURS carries E2E Profile 11.
 * Both have a 100 ms timeout. Frames are sent 20 ms apart unless a test says
 * otherwise.
 */
#include <string.h>

#include "e2e_p11.h"
#include "mailbox.h"
#include "unity.h"

static const mailbox_config_t PLAIN = {
    .frame_id = 0x0AAu,
    .length = 8u,
    .timeout_ms = 100u,
    .substitute = {0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B},
};

static const mailbox_config_t OURS = {
    .frame_id = 0x080u,
    .length = 8u,
    .e2e = true,
    .e2e_data_id = 0x0080u,
    .timeout_ms = 100u,
    .substitute = {0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static mailbox_rx_t rx;
static mailbox_t mb;
static e2e_p11_tx_t sender;

void setUp(void)
{
    memset(&rx, 0, sizeof rx);
    memset(&mb, 0, sizeof mb);
    memset(&sender, 0, sizeof sender);
}

void tearDown(void) {}

/* A PLAIN frame whose bytes are all `fill` arrives at `at` and a step runs. */
static mailbox_state_t plain_frame(uint8_t fill, uint32_t at)
{
    uint8_t frame[8];

    memset(frame, fill, sizeof frame);
    mailbox_rx_store(&rx, frame, sizeof frame, at);
    return mailbox_update(&mb, &PLAIN, &rx, at);
}

/* The sender's next OURS frame, with `fill` in its signal bytes. */
static void build_ours(uint8_t frame[8], uint8_t fill)
{
    memset(frame, fill, 8);
    frame[1] = 0;
    e2e_p11_protect(&sender, OURS.e2e_data_id, frame, 8);
}

/* The sender's next OURS frame arrives at `at` and a step runs. */
static mailbox_state_t ours_frame(uint8_t fill, uint32_t at)
{
    uint8_t frame[8];

    build_ours(frame, fill);
    mailbox_rx_store(&rx, frame, sizeof frame, at);
    return mailbox_update(&mb, &OURS, &rx, at);
}

/* A step with no new frame. */
static mailbox_state_t step(const mailbox_config_t *config, uint32_t now)
{
    return mailbox_update(&mb, config, &rx, now);
}

static void assert_reads(const mailbox_config_t *config, uint8_t fill)
{
    uint8_t expected[8];

    memset(expected, fill, sizeof expected);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, mailbox_read(&mb, config), 8);
}

static void assert_reads_substitute(const mailbox_config_t *config)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(config->substitute, mailbox_read(&mb, config), 8);
}

/* ---- NODATA to VALID ---------------------------------------------------- */

static void test_a_mailbox_that_has_heard_nothing_is_nodata_and_reads_the_substitute(void)
{
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&PLAIN, 0));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&PLAIN, 100000));
    assert_reads_substitute(&PLAIN);
    TEST_ASSERT_EQUAL_UINT32(0, mb.counters.timeouts);
}

static void test_valid_from_nodata_needs_exactly_three_good_frames(void)
{
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x11, 1000));
    assert_reads_substitute(&PLAIN);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x22, 1020));
    assert_reads_substitute(&PLAIN);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x33, 1040));
    assert_reads(&PLAIN, 0x33);
    TEST_ASSERT_EQUAL_UINT32(3, mb.counters.good);
}

static void test_the_same_frame_seen_by_several_steps_counts_once(void)
{
    plain_frame(0x11, 1000);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&PLAIN, 1010));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&PLAIN, 1019));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x22, 1020));
    TEST_ASSERT_EQUAL_UINT32(2, mb.counters.good);
}

static void test_a_newer_frame_replaces_the_older_one(void)
{
    plain_frame(0x11, 1000);
    plain_frame(0x22, 1020);
    plain_frame(0x33, 1040);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x44, 1060));
    assert_reads(&PLAIN, 0x44);
}

static void test_frames_replaced_before_a_step_saw_them_are_counted_not_checked(void)
{
    const uint8_t a[8] = {0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1, 0xA1};
    const uint8_t b[8] = {0xB2, 0xB2, 0xB2, 0xB2, 0xB2, 0xB2, 0xB2, 0xB2};
    const uint8_t c[8] = {0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3};

    mailbox_rx_store(&rx, a, sizeof a, 1000);
    mailbox_rx_store(&rx, b, sizeof b, 1001);
    mailbox_rx_store(&rx, c, sizeof c, 1002);

    /* Three frames arrived but the step saw one: one good frame, not three. */
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&PLAIN, 1005));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.good);
    TEST_ASSERT_EQUAL_UINT32(2, mb.counters.overwritten);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(c, rx.data, sizeof c);
    TEST_ASSERT_EQUAL_UINT32(1002, rx.arrival_ms);
}

/* ---- The timeout -------------------------------------------------------- */

static void make_plain_valid_with_last_frame_at(uint32_t last)
{
    plain_frame(0x11, last - 40u);
    plain_frame(0x22, last - 20u);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x33, last));
}

static void test_valid_lasts_until_one_millisecond_short_of_the_timeout(void)
{
    make_plain_valid_with_last_frame_at(1000);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 1099));
    assert_reads(&PLAIN, 0x33);
    TEST_ASSERT_EQUAL_UINT32(0, mb.counters.timeouts);
}

static void test_invalid_at_exactly_the_timeout_and_the_reader_gets_the_substitute(void)
{
    make_plain_valid_with_last_frame_at(1000);

    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1100));
    assert_reads_substitute(&PLAIN);
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.timeouts);

    /* It stays INVALID and the timeout is counted once. */
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1110));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.timeouts);
}

static void test_each_good_frame_restarts_the_timeout(void)
{
    make_plain_valid_with_last_frame_at(1000);
    plain_frame(0x44, 1099);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 1198));
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1199));
}

static void test_valid_from_invalid_needs_exactly_three_good_frames(void)
{
    make_plain_valid_with_last_frame_at(1000);
    step(&PLAIN, 1100);

    TEST_ASSERT_EQUAL(MAILBOX_INVALID, plain_frame(0x44, 1200));
    assert_reads_substitute(&PLAIN);
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, plain_frame(0x55, 1220));
    assert_reads_substitute(&PLAIN);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x66, 1240));
    assert_reads(&PLAIN, 0x66);
}

static void test_a_gap_of_the_timeout_between_good_frames_starts_the_run_again(void)
{
    plain_frame(0x11, 1000);
    plain_frame(0x22, 1020);
    /* 100 ms after the second frame: not "in a row". */
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x33, 1120));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x44, 1140));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x55, 1160));
    /* Never VALID before, so nothing timed out. */
    TEST_ASSERT_EQUAL_UINT32(0, mb.counters.timeouts);
}

static void test_a_gap_one_short_of_the_timeout_keeps_the_run(void)
{
    plain_frame(0x11, 1000);
    plain_frame(0x22, 1020);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x33, 1119));
}

static void test_a_late_step_cannot_hide_a_timeout_between_two_frames(void)
{
    make_plain_valid_with_last_frame_at(1000);

    /* No step ran from 1000 to 1150, and a frame arrived at 1140. The gap
     * was 140 ms, so the mailbox timed out even though the frame is fresh. */
    {
        uint8_t frame[8];
        memset(frame, 0x44, sizeof frame);
        mailbox_rx_store(&rx, frame, sizeof frame, 1140);
    }
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1150));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.timeouts);
    assert_reads_substitute(&PLAIN);
}

static void test_a_frame_stamped_just_after_the_step_time_has_age_zero(void)
{
    uint8_t frame[8];

    make_plain_valid_with_last_frame_at(1000);
    /* The interrupt ran between reading the clock (1049) and the snapshot. */
    memset(frame, 0x44, sizeof frame);
    mailbox_rx_store(&rx, frame, sizeof frame, 1050);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 1049));
    assert_reads(&PLAIN, 0x44);
}

/* ---- The 32-bit millisecond wrap ---------------------------------------- */

static void test_valid_is_reached_across_the_wrap(void)
{
    plain_frame(0x11, 0xFFFFFFECu); /* 20 ms before the wrap */
    plain_frame(0x22, 0x00000000u);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x33, 0x00000014u));
    assert_reads(&PLAIN, 0x33);
}

static void test_the_timeout_edge_is_exact_across_the_wrap(void)
{
    make_plain_valid_with_last_frame_at(0xFFFFFFCEu); /* 50 ms before the wrap */

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 0x00000000u));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 0x00000031u)); /* age 99 */
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 0x00000032u)); /* age 100 */
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.timeouts);
}

/* ---- Frames that fail a check ------------------------------------------- */

static void test_a_short_frame_is_dropped_and_counted_and_breaks_the_run(void)
{
    const uint8_t frame[8] = {0};

    plain_frame(0x11, 1000);
    plain_frame(0x22, 1020);
    mailbox_rx_store(&rx, frame, 7, 1040);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&PLAIN, 1040));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_length);

    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x33, 1060));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x44, 1080));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x55, 1100));
}

static void test_our_frames_reach_valid_after_three_with_good_crc_and_counter(void)
{
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, ours_frame(0x11, 1000));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, ours_frame(0x22, 1020));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x33, 1040));
    TEST_ASSERT_EQUAL_HEX8(0x33, mailbox_read(&mb, &OURS)[7]);
}

static void make_ours_valid_with_last_frame_at(uint32_t last)
{
    ours_frame(0x11, last - 40u);
    ours_frame(0x22, last - 20u);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x33, last));
}

static void test_a_bad_crc_is_dropped_and_counted_and_the_reader_keeps_the_last_good_frame(void)
{
    uint8_t frame[8];

    make_ours_valid_with_last_frame_at(1000);
    build_ours(frame, 0x44);
    frame[7] ^= 0x01u; /* one bit flipped on the wire */
    mailbox_rx_store(&rx, frame, sizeof frame, 1020);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&OURS, 1020));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_crc);
    TEST_ASSERT_EQUAL_HEX8(0x33, mailbox_read(&mb, &OURS)[7]);
}

static void test_a_repeated_counter_is_dropped_and_counted(void)
{
    uint8_t frame[8];

    make_ours_valid_with_last_frame_at(1000);
    memcpy(frame, rx.data, sizeof frame);
    mailbox_rx_store(&rx, frame, sizeof frame, 1020); /* the same frame again */

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&OURS, 1020));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_counter);
    TEST_ASSERT_EQUAL_UINT32(3, mb.counters.good);
}

static void test_a_skipped_counter_is_dropped_and_counted(void)
{
    uint8_t lost[8];

    make_ours_valid_with_last_frame_at(1000);
    build_ours(lost, 0x44); /* never arrives */

    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x55, 1040));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_counter);
    TEST_ASSERT_EQUAL_HEX8(0x33, mailbox_read(&mb, &OURS)[7]);
}

static void test_dropped_frames_never_make_a_mailbox_invalid_only_the_timeout_does(void)
{
    uint8_t frame[8];

    make_ours_valid_with_last_frame_at(1000);

    /* A stuck sender repeats its last frame every 20 ms. Every one is
     * dropped; the mailbox is VALID until the last good frame is 100 ms old. */
    memcpy(frame, rx.data, sizeof frame);
    for (uint32_t t = 1020; t <= 1080; t += 20) {
        mailbox_rx_store(&rx, frame, sizeof frame, t);
        TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&OURS, t));
    }
    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&OURS, 1099));
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&OURS, 1100));
    TEST_ASSERT_EQUAL_UINT32(4, mb.counters.bad_counter);
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.timeouts);
    assert_reads_substitute(&OURS);
}

static void test_a_dropped_frame_does_not_restart_the_timeout(void)
{
    uint8_t frame[8];

    make_ours_valid_with_last_frame_at(1000);
    build_ours(frame, 0x44);
    frame[0] ^= 0xFFu;
    mailbox_rx_store(&rx, frame, sizeof frame, 1090);
    step(&OURS, 1090);

    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&OURS, 1100));
}

static void test_a_bad_frame_on_the_way_to_valid_starts_the_run_again(void)
{
    uint8_t frame[8];

    ours_frame(0x11, 1000);
    ours_frame(0x22, 1020);
    build_ours(frame, 0x33);
    frame[3] ^= 0x80u;
    mailbox_rx_store(&rx, frame, sizeof frame, 1040);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, step(&OURS, 1040));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_crc);

    /* The frame after it is dropped as well: its counter is two past the
     * last good one (e2e_p11.h). Then three good frames. */
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, ours_frame(0x44, 1060));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_counter);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, ours_frame(0x55, 1080));
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, ours_frame(0x66, 1100));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x77, 1120));
}

static void test_after_a_timeout_the_sender_counter_may_be_anywhere(void)
{
    uint8_t unheard[8];

    make_ours_valid_with_last_frame_at(1000);
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&OURS, 1100));

    /* The sender carried on while we could not hear it. */
    for (int i = 0; i < 7; i++) {
        build_ours(unheard, 0x00);
    }

    /* The first frame heard again is the new reference: three frames, not four. */
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, ours_frame(0x44, 1300));
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, ours_frame(0x55, 1320));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x66, 1340));
    TEST_ASSERT_EQUAL_UINT32(0, mb.counters.bad_counter);
}

static void test_a_bad_frame_on_the_way_back_from_invalid_starts_the_run_again(void)
{
    const uint8_t short_frame[8] = {0};

    make_plain_valid_with_last_frame_at(1000);
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1100));

    plain_frame(0x44, 1200);
    plain_frame(0x55, 1220);
    mailbox_rx_store(&rx, short_frame, 7, 1240);
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1240));

    TEST_ASSERT_EQUAL(MAILBOX_INVALID, plain_frame(0x66, 1260));
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, plain_frame(0x77, 1280));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x88, 1300));
}

static void test_a_short_frame_while_valid_is_dropped_and_the_mailbox_stays_valid(void)
{
    const uint8_t short_frame[8] = {0};

    make_plain_valid_with_last_frame_at(1000);
    mailbox_rx_store(&rx, short_frame, 7, 1020);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&PLAIN, 1020));
    TEST_ASSERT_EQUAL_UINT32(1, mb.counters.bad_length);
    assert_reads(&PLAIN, 0x33);
    /* It did not restart the timeout either. */
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1100));
}

static void test_our_frames_are_checked_and_aged_across_the_wrap(void)
{
    make_ours_valid_with_last_frame_at(0xFFFFFFF6u); /* 10 ms before the wrap */
    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x44, 0x0000000Au));
    TEST_ASSERT_EQUAL_HEX8(0x44, mailbox_read(&mb, &OURS)[7]);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, step(&OURS, 0x0000006Du));   /* age 99 */
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&OURS, 0x0000006Eu)); /* age 100 */
}

static void test_the_gap_between_good_frames_is_measured_across_the_wrap(void)
{
    plain_frame(0x11, 0xFFFFFFB0u);
    plain_frame(0x22, 0xFFFFFFCEu); /* 50 ms before the wrap */
    /* 99 ms later: still in a row. */
    TEST_ASSERT_EQUAL(MAILBOX_VALID, plain_frame(0x33, 0x00000031u));

    setUp();
    plain_frame(0x11, 0xFFFFFFB0u);
    plain_frame(0x22, 0xFFFFFFCEu);
    /* 100 ms later: the run starts again. */
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, plain_frame(0x33, 0x00000032u));
}

/* ---- Forced INVALID (bus-off) ------------------------------------------- */

static void test_a_forced_mailbox_is_invalid_at_once_and_reads_the_substitute(void)
{
    make_plain_valid_with_last_frame_at(1000);

    mailbox_force_invalid(&mb);

    TEST_ASSERT_EQUAL(MAILBOX_INVALID, mb.state);
    assert_reads_substitute(&PLAIN);
    /* The frame from before the bus-off does not bring it back. */
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, step(&PLAIN, 1001));
    TEST_ASSERT_EQUAL_UINT32(0, mb.counters.timeouts);
}

static void test_a_forced_mailbox_needs_three_new_good_frames(void)
{
    uint8_t unheard[8];

    make_ours_valid_with_last_frame_at(1000);
    mailbox_force_invalid(&mb);
    build_ours(unheard, 0x00);
    build_ours(unheard, 0x00);

    TEST_ASSERT_EQUAL(MAILBOX_INVALID, ours_frame(0x44, 1020));
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, ours_frame(0x55, 1040));
    TEST_ASSERT_EQUAL(MAILBOX_VALID, ours_frame(0x66, 1060));
}

static void test_every_mailbox_is_forced_invalid_at_once_whatever_its_state(void)
{
    mailbox_t all[3];

    memset(all, 0, sizeof all);
    all[0].state = MAILBOX_NODATA;
    make_plain_valid_with_last_frame_at(1000);
    all[1] = mb; /* VALID */
    step(&PLAIN, 1100);
    all[2] = mb; /* INVALID */

    mailbox_force_all_invalid(all, 3);

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(MAILBOX_INVALID, all[i].state);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(PLAIN.substitute, mailbox_read(&all[i], &PLAIN), 8);
    }
}

/* ---- The interrupt's half ----------------------------------------------- */

static void test_storing_a_frame_longer_than_eight_bytes_keeps_eight(void)
{
    uint8_t frame[12];

    memset(frame, 0x77, sizeof frame);
    mailbox_rx_store(&rx, frame, sizeof frame, 1000);

    TEST_ASSERT_EQUAL_UINT8(8, rx.len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_mailbox_that_has_heard_nothing_is_nodata_and_reads_the_substitute);
    RUN_TEST(test_valid_from_nodata_needs_exactly_three_good_frames);
    RUN_TEST(test_the_same_frame_seen_by_several_steps_counts_once);
    RUN_TEST(test_a_newer_frame_replaces_the_older_one);
    RUN_TEST(test_frames_replaced_before_a_step_saw_them_are_counted_not_checked);
    RUN_TEST(test_valid_lasts_until_one_millisecond_short_of_the_timeout);
    RUN_TEST(test_invalid_at_exactly_the_timeout_and_the_reader_gets_the_substitute);
    RUN_TEST(test_each_good_frame_restarts_the_timeout);
    RUN_TEST(test_valid_from_invalid_needs_exactly_three_good_frames);
    RUN_TEST(test_a_gap_of_the_timeout_between_good_frames_starts_the_run_again);
    RUN_TEST(test_a_gap_one_short_of_the_timeout_keeps_the_run);
    RUN_TEST(test_a_late_step_cannot_hide_a_timeout_between_two_frames);
    RUN_TEST(test_a_frame_stamped_just_after_the_step_time_has_age_zero);
    RUN_TEST(test_valid_is_reached_across_the_wrap);
    RUN_TEST(test_the_timeout_edge_is_exact_across_the_wrap);
    RUN_TEST(test_a_short_frame_is_dropped_and_counted_and_breaks_the_run);
    RUN_TEST(test_our_frames_reach_valid_after_three_with_good_crc_and_counter);
    RUN_TEST(test_a_bad_crc_is_dropped_and_counted_and_the_reader_keeps_the_last_good_frame);
    RUN_TEST(test_a_repeated_counter_is_dropped_and_counted);
    RUN_TEST(test_a_skipped_counter_is_dropped_and_counted);
    RUN_TEST(test_dropped_frames_never_make_a_mailbox_invalid_only_the_timeout_does);
    RUN_TEST(test_a_dropped_frame_does_not_restart_the_timeout);
    RUN_TEST(test_a_bad_frame_on_the_way_to_valid_starts_the_run_again);
    RUN_TEST(test_after_a_timeout_the_sender_counter_may_be_anywhere);
    RUN_TEST(test_a_bad_frame_on_the_way_back_from_invalid_starts_the_run_again);
    RUN_TEST(test_a_short_frame_while_valid_is_dropped_and_the_mailbox_stays_valid);
    RUN_TEST(test_our_frames_are_checked_and_aged_across_the_wrap);
    RUN_TEST(test_the_gap_between_good_frames_is_measured_across_the_wrap);
    RUN_TEST(test_a_forced_mailbox_is_invalid_at_once_and_reads_the_substitute);
    RUN_TEST(test_a_forced_mailbox_needs_three_new_good_frames);
    RUN_TEST(test_every_mailbox_is_forced_invalid_at_once_whatever_its_state);
    RUN_TEST(test_storing_a_frame_longer_than_eight_bytes_keeps_eight);
    return UNITY_END();
}
