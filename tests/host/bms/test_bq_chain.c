/*
 * Host tests for the cell-monitor chain's sequencer (App/bms/logic/Inc/bq_chain.h).
 *
 * Issue 62:
 * - "The first chain command on every boot is a balancing stop."
 * - "Wake and addressing are spread over steps and never block longer than
 *   the kick spacing allows; a chain that does not wake is retried once per
 *   second indefinitely."
 * - "One sweep per step; a link timeout is reported as a missing
 *   measurement."
 *
 * The wake and addressing commands are TI's (BQ79600-Q1 Software Design
 * Reference SLUAA17A 2.1, 2.2); the read timeout is tWAIT_READ_MAX (BQ79600-Q1
 * data sheet SLUSDS1A p. 23), which the research for issue 3 works out as
 * 7.23 ms for the Sweep.
 */
#include <string.h>

#include "bq_chain.h"
#include "bq_frame.h"
#include "bq_sweep.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define STEP_MS 20u

static bq_chain_t chain;
static bq_plan_t plan;
static bq_sweep_t sweep;
static uint8_t resp[BQ_SWEEP_RESPONSE_LEN];

/* The command at i is exactly this frame. */
static void assert_write(size_t i, bq_request_t type, uint8_t dev, uint16_t reg,
                         const uint8_t *data, size_t n)
{
    uint8_t expected[BQ_FRAME_MAX];
    const size_t len = bq_frame_write(expected, type, dev, reg, data, n);

    TEST_ASSERT_TRUE(i < plan.count);
    TEST_ASSERT_EQUAL_UINT(len, plan.cmd[i].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, plan.cmd[i].frame, len);
    TEST_ASSERT_EQUAL_UINT(0u, plan.cmd[i].response_len);
}

static void assert_read(size_t i, uint16_t reg, size_t count)
{
    uint8_t expected[BQ_FRAME_MAX];
    const size_t len = bq_frame_read(expected, BQ_STACK_READ, 0u, reg, count);

    TEST_ASSERT_TRUE(i < plan.count);
    TEST_ASSERT_EQUAL_UINT(len, plan.cmd[i].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, plan.cmd[i].frame, len);
    TEST_ASSERT_EQUAL_UINT(16u * (count + 6u), plan.cmd[i].response_len);
}

static size_t put_frame(uint8_t *buf, uint8_t dev, uint16_t reg, const uint8_t *data, size_t n)
{
    buf[0] = (uint8_t)(n - 1u);
    buf[1] = dev;
    buf[2] = (uint8_t)(reg >> 8);
    buf[3] = (uint8_t)reg;
    memcpy(&buf[4], data, n);
    const uint16_t crc = bq_crc(buf, 4u + n);
    buf[4u + n] = (uint8_t)crc;
    buf[5u + n] = (uint8_t)(crc >> 8);
    return 6u + n;
}

/* What the stack answers to the address read-back: each device its own
   address, top first. top is the address the top device reports. */
static size_t address_readback(uint8_t top)
{
    size_t len = 0u;
    for (uint8_t dev = 16u; dev >= 1u; dev--) {
        const uint8_t addr = (dev == 16u) ? top : dev;
        len += put_frame(&resp[len], addr, 0x0306u, &addr, 1u);
    }
    return len;
}

/* Every device answers the Sweep with zeros. */
static size_t sweep_response(void)
{
    uint8_t data[BQ_SWEEP_COUNT] = {0};
    size_t len = 0u;
    for (uint8_t dev = 16u; dev >= 1u; dev--) {
        len += put_frame(&resp[len], dev, BQ_SWEEP_REG, data, sizeof data);
    }
    return len;
}

/* Plans the step at now and reports it done, every command answered in
   full; for the address read-back, with a good answer. Returns the step's
   Sweep flag. */
static bool step_ok(uint32_t now)
{
    bq_chain_plan(&chain, now, &plan);
    size_t len = 0u;
    if (bq_chain_state(&chain) == BQ_CHAIN_VERIFY) {
        len = address_readback(16u);
    } else if (bq_chain_state(&chain) == BQ_CHAIN_RUNNING) {
        len = sweep_response();
    }
    return bq_chain_done(&chain, now, true, resp, len, &sweep);
}

/* Steps from now until the chain runs; returns the time of the last step. */
static uint32_t wake_from(uint32_t now)
{
    for (int i = 0; i < 100 && !bq_chain_running(&chain); i++) {
        (void)step_ok(now);
        now += STEP_MS;
    }
    TEST_ASSERT_TRUE(bq_chain_running(&chain));
    return now - STEP_MS;
}

/* Steps from 0 ms until the chain is in state; returns the time of the
   next step. Fails rather than hangs if it never gets there. */
static uint32_t step_until(bq_chain_state_t state)
{
    uint32_t now = 0u;
    for (int i = 0; i < 100 && bq_chain_state(&chain) != state; i++) {
        (void)step_ok(now);
        now += STEP_MS;
    }
    TEST_ASSERT_EQUAL(state, bq_chain_state(&chain));
    return now;
}

/* Steps through the stack's wake and plans the first step that sends
   anything up the chain; returns its time. */
static uint32_t plan_first_chain_step(void)
{
    uint32_t now = step_until(BQ_CHAIN_STACK_WAIT);
    for (int i = 0; i < 20; i++, now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        if (plan.count > 0u) {
            return now;
        }
        (void)bq_chain_done(&chain, now, true, resp, 0u, &sweep);
    }
    TEST_FAIL_MESSAGE("nothing went up the chain");
    return now;
}

static void test_the_first_step_sends_the_wake_ping_and_nothing_else(void)
{
    bq_chain_init(&chain);
    bq_chain_plan(&chain, 0u, &plan);

    TEST_ASSERT_TRUE(plan.wake_ping);
    TEST_ASSERT_EQUAL_UINT(0u, plan.count);
    TEST_ASSERT_FALSE(bq_chain_running(&chain));
}

static void test_the_next_step_wakes_the_stack_through_the_bridge(void)
{
    const uint8_t sendWake[] = {0x20};

    bq_chain_init(&chain);
    (void)step_ok(0u);
    bq_chain_plan(&chain, STEP_MS, &plan);

    TEST_ASSERT_FALSE(plan.wake_ping);
    TEST_ASSERT_EQUAL_UINT(1u, plan.count);
    assert_write(0u, BQ_SINGLE_WRITE, 0u, 0x0309u, sendWake, 1u);
}

static void test_nothing_goes_up_the_stack_until_its_wake_tone_has_passed(void)
{
    /* SLUAA17A 2.1 step 4: (1.6 ms + 10 ms) x 16 devices = 185.6 ms. */
    bq_chain_init(&chain);
    (void)step_ok(0u);
    (void)step_ok(20u);

    for (uint32_t now = 40u; now < 20u + 186u; now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        TEST_ASSERT_EQUAL_UINT(0u, plan.count);
        TEST_ASSERT_FALSE(plan.wake_ping);
        (void)bq_chain_done(&chain, now, true, resp, 0u, &sweep);
    }
    bq_chain_plan(&chain, 240u, &plan);
    TEST_ASSERT_TRUE(plan.count > 0u);
}

static void test_the_first_chain_command_is_a_balancing_stop(void)
{
    /* Every balancing timer to 0 s with BAL_GO (SLUSE81F 7.3.3.3.2), as a
       broadcast, since the stack is not addressed yet. */
    const uint8_t zeros[8] = {0};
    const uint8_t balGo[] = {0x02};

    bq_chain_init(&chain);
    (void)plan_first_chain_step();

    assert_write(0u, BQ_BROADCAST_WRITE, 0u, 0x0318u, zeros, 8u); /* CB_CELL16..9_CTRL */
    assert_write(1u, BQ_BROADCAST_WRITE, 0u, 0x0320u, zeros, 8u); /* CB_CELL8..1_CTRL */
    assert_write(2u, BQ_BROADCAST_WRITE, 0u, 0x032Fu, balGo, 1u); /* BAL_CTRL2 */
}

static void test_auto_addressing_follows_ti_for_sixteen_devices(void)
{
    const uint8_t zero[] = {0x00};
    const uint8_t one[] = {0x01};
    const uint8_t two[] = {0x02};
    const uint8_t three[] = {0x03};

    bq_chain_init(&chain);
    const uint32_t now = plan_first_chain_step();

    /* After the balancing stop: SLUAA17A 2.2.1 steps 1 to 5. */
    for (uint16_t i = 0u; i < 8u; i++) {
        assert_write(3u + i, BQ_STACK_WRITE, 0u, (uint16_t)(0x0343u + i), zero, 1u);
    }
    assert_write(11u, BQ_BROADCAST_WRITE, 0u, 0x0309u, one, 1u);
    for (uint8_t a = 0u; a <= 16u; a++) {
        assert_write(12u + a, BQ_BROADCAST_WRITE, 0u, 0x0306u, &a, 1u);
    }
    assert_write(29u, BQ_BROADCAST_WRITE, 0u, 0x0308u, two, 1u);
    assert_write(30u, BQ_SINGLE_WRITE, 16u, 0x0308u, three, 1u);
    TEST_ASSERT_EQUAL_UINT(31u, plan.count);
    (void)bq_chain_done(&chain, now, true, resp, 0u, &sweep);

    /* Step 6, on the next step: the DLL sync reads. */
    bq_chain_plan(&chain, now + STEP_MS, &plan);
    TEST_ASSERT_EQUAL_UINT(8u, plan.count);
    for (uint16_t i = 0u; i < 8u; i++) {
        assert_read(i, (uint16_t)(0x0343u + i), 1u);
    }
}

static void test_the_addresses_are_read_back_then_the_adc_is_started(void)
{
    const uint8_t eightCells[] = {0x02};
    const uint8_t gpioAdc[] = {0x12, 0x12, 0x12, 0x12};
    const uint8_t tsref[] = {0x01};
    const uint8_t continuous[] = {0x06};

    bq_chain_init(&chain);
    uint32_t now = step_until(BQ_CHAIN_VERIFY);
    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_EQUAL_UINT(1u, plan.count);
    assert_read(0u, 0x0306u, 1u);
    (void)bq_chain_done(&chain, now, true, resp, address_readback(16u), &sweep);

    bq_chain_plan(&chain, now + STEP_MS, &plan);
    TEST_ASSERT_EQUAL_UINT(4u, plan.count);
    assert_write(0u, BQ_STACK_WRITE, 0u, 0x0003u, eightCells, 1u); /* ACTIVE_CELL 8S */
    assert_write(1u, BQ_STACK_WRITE, 0u, 0x000Eu, gpioAdc, 4u);    /* GPIO1-8 ADC only */
    assert_write(2u, BQ_STACK_WRITE, 0u, 0x030Au, tsref, 1u);      /* TSREF_EN: sensor bias */
    assert_write(3u, BQ_STACK_WRITE, 0u, 0x030Du, continuous, 1u); /* MAIN_GO, continuous */
    (void)bq_chain_done(&chain, now + STEP_MS, true, resp, 0u, &sweep);

    TEST_ASSERT_TRUE(bq_chain_running(&chain));
}

static void test_a_running_chain_takes_one_sweep_per_step(void)
{
    bq_chain_init(&chain);
    uint32_t now = wake_from(0u) + STEP_MS;

    for (int i = 0; i < 5; i++, now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        TEST_ASSERT_FALSE(plan.wake_ping);
        TEST_ASSERT_EQUAL_UINT(1u, plan.count);
        assert_read(0u, BQ_SWEEP_REG, BQ_SWEEP_COUNT);
        TEST_ASSERT_TRUE(bq_chain_done(&chain, now, true, resp, sweep_response(), &sweep));
        TEST_ASSERT_EQUAL_HEX16(0xFFFFu, sweep.devices_read);
    }
}

static void test_a_sweep_that_times_out_is_a_missing_measurement(void)
{
    bq_chain_init(&chain);
    const uint32_t now = wake_from(0u) + STEP_MS;
    (void)sweep_response();

    bq_chain_plan(&chain, now, &plan);
    /* The read timed out with five devices' frames in. */
    TEST_ASSERT_TRUE(bq_chain_done(&chain, now, false, resp, 5u * 44u, &sweep));
    TEST_ASSERT_EQUAL_HEX16(0xF800u, sweep.devices_read);
    TEST_ASSERT_TRUE(bq_chain_running(&chain));
}

static void test_a_chain_that_does_not_answer_is_woken_again_a_second_after_the_last_try(void)
{
    bq_chain_init(&chain);
    uint32_t now = step_until(BQ_CHAIN_VERIFY);
    /* No stack: the read-back times out with nothing. */
    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_FALSE(bq_chain_done(&chain, now, false, resp, 0u, &sweep));
    TEST_ASSERT_EQUAL_UINT(1u, bq_chain_attempts(&chain));

    /* Quiet until a second after the first ping, at 0 ms. */
    for (now += STEP_MS; now < 1000u; now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        TEST_ASSERT_FALSE(plan.wake_ping);
        TEST_ASSERT_EQUAL_UINT(0u, plan.count);
        (void)bq_chain_done(&chain, now, true, resp, 0u, &sweep);
    }
    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_TRUE(plan.wake_ping);
    (void)bq_chain_done(&chain, now, true, resp, 0u, &sweep);
    TEST_ASSERT_EQUAL_UINT(2u, bq_chain_attempts(&chain));
}

static void test_retries_go_on_for_ever(void)
{
    bq_chain_init(&chain);
    uint32_t now = 0u;
    uint32_t pings = 0u;

    /* A chain that never answers anything, for a minute. */
    for (; now < 60000u; now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        if (plan.wake_ping) {
            pings++;
        }
        (void)bq_chain_done(&chain, now, plan.count == 0u, resp, 0u, &sweep);
    }
    TEST_ASSERT_UINT_WITHIN(1u, 60u, pings);
}

static void test_a_wrong_address_read_back_is_a_failed_wake(void)
{
    bq_chain_init(&chain);
    uint32_t now = step_until(BQ_CHAIN_VERIFY);
    bq_chain_plan(&chain, now, &plan);
    (void)bq_chain_done(&chain, now, true, resp, address_readback(17u), &sweep);

    TEST_ASSERT_EQUAL(BQ_CHAIN_RETRY_WAIT, bq_chain_state(&chain));
}

static void test_a_chain_that_stops_answering_for_a_second_is_woken_again(void)
{
    bq_chain_init(&chain);
    uint32_t now = wake_from(0u) + STEP_MS;

    for (int i = 0; i < 49; i++, now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        (void)bq_chain_done(&chain, now, false, resp, 0u, &sweep);
        TEST_ASSERT_TRUE(bq_chain_running(&chain));
    }
    bq_chain_plan(&chain, now, &plan);
    (void)bq_chain_done(&chain, now, false, resp, 0u, &sweep);
    TEST_ASSERT_FALSE(bq_chain_running(&chain));

    bq_chain_plan(&chain, now + STEP_MS, &plan);
    TEST_ASSERT_TRUE(plan.wake_ping);
}

static void test_the_sweep_read_waits_its_tWAIT_READ_MAX(void)
{
    TEST_ASSERT_EQUAL_UINT32(7230u, bq_read_timeout_us(BQ_SWEEP_RESPONSE_LEN));
}

static void test_no_step_of_the_wake_or_the_sweep_can_outlast_its_budget(void)
{
    bq_chain_init(&chain);
    uint32_t now = 0u;

    for (int i = 0; i < 40; i++, now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        TEST_ASSERT_TRUE(bq_plan_worst_us(&plan) <= BQ_PLAN_BUDGET_US);
        size_t len = 0u;
        if (bq_chain_state(&chain) == BQ_CHAIN_VERIFY) {
            len = address_readback(16u);
        } else if (bq_chain_state(&chain) == BQ_CHAIN_RUNNING) {
            len = sweep_response();
        }
        (void)bq_chain_done(&chain, now, true, resp, len, &sweep);
    }
    TEST_ASSERT_TRUE(bq_chain_running(&chain));
    /* Inside the 20 ms step, so the next Kick comes on time. */
    TEST_ASSERT_TRUE(BQ_PLAN_BUDGET_US < 20000u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_step_sends_the_wake_ping_and_nothing_else);
    RUN_TEST(test_the_next_step_wakes_the_stack_through_the_bridge);
    RUN_TEST(test_nothing_goes_up_the_stack_until_its_wake_tone_has_passed);
    RUN_TEST(test_the_first_chain_command_is_a_balancing_stop);
    RUN_TEST(test_auto_addressing_follows_ti_for_sixteen_devices);
    RUN_TEST(test_the_addresses_are_read_back_then_the_adc_is_started);
    RUN_TEST(test_a_running_chain_takes_one_sweep_per_step);
    RUN_TEST(test_a_sweep_that_times_out_is_a_missing_measurement);
    RUN_TEST(test_a_chain_that_does_not_answer_is_woken_again_a_second_after_the_last_try);
    RUN_TEST(test_retries_go_on_for_ever);
    RUN_TEST(test_a_wrong_address_read_back_is_a_failed_wake);
    RUN_TEST(test_a_chain_that_stops_answering_for_a_second_is_woken_again);
    RUN_TEST(test_the_sweep_read_waits_its_tWAIT_READ_MAX);
    RUN_TEST(test_no_step_of_the_wake_or_the_sweep_can_outlast_its_budget);
    return UNITY_END();
}
