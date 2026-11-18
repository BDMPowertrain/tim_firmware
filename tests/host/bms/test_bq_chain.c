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
 *
 * Issue 64 (spec #36, "BMS image"):
 * - "Comparators enabled: over-voltage 4.225 V, under-voltage 2.4 V,
 *   over-temperature at the nearest step above 60 C; the chip's
 *   stop-on-fault is enabled; post-wake faults are cleared before BMS_OK
 *   may rise." Register values from the BQ79616-Q1 data sheet SLUSE81F
 *   7.5.4 and the BQ79600-Q1 data sheet SLUSDS1A 7.5.
 * - Each Sweep reads the bridge's fault register, so the NFAULT read-back
 *   can be checked against it.
 * - The open-wire check (EV.8.3.4 b, "voltage sense ... blown or
 *   tripped"; SLUSE81F 7.3.6.4.6.4): every VC pin's current sink on, wait
 *   for an open wire's filter capacitor to drain, compare every cell with
 *   the threshold, sinks off, then read whether each device finished and
 *   what it found.
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
static uint8_t resp[BQ_RESPONSE_MAX];

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

/* The command at i is a single read of count bytes from the bridge,
   appended to the read before it. */
static void assert_bridge_read(size_t i, uint16_t reg, size_t count)
{
    uint8_t expected[BQ_FRAME_MAX];
    const size_t len = bq_frame_read(expected, BQ_SINGLE_READ, 0u, reg, count);

    TEST_ASSERT_TRUE(i < plan.count);
    TEST_ASSERT_EQUAL_UINT(len, plan.cmd[i].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, plan.cmd[i].frame, len);
    TEST_ASSERT_EQUAL_UINT(count + 6u, plan.cmd[i].response_len);
    TEST_ASSERT_TRUE(plan.cmd[i].append);
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

/* Every device answers the Sweep with zeros, and the bridge's
   FAULT_SUMMARY (0x2100) is summary. */
static size_t sweep_response_with(uint8_t summary)
{
    uint8_t data[BQ_SWEEP_COUNT] = {0};
    size_t len = 0u;
    for (uint8_t dev = 16u; dev >= 1u; dev--) {
        len += put_frame(&resp[len], dev, BQ_SWEEP_REG, data, sizeof data);
    }
    len += put_frame(&resp[len], 0u, 0x2100u, &summary, 1u);
    return len;
}

static size_t sweep_response(void)
{
    return sweep_response_with(0u);
}

/* The open-wire check's answer: each device's ADC_STAT2 (0x0528), top
   first, then each device's FAULT_COMP_VCOW1..2 (0x0548), top first.
   stat[d - 1] and vcow2[d - 1] are device d's; a device in missing
   answers neither. */
static size_t open_wire_response(const uint8_t stat[16], const uint8_t vcow2[16],
                                 uint16_t missing)
{
    size_t len = 0u;
    for (uint8_t dev = 16u; dev >= 1u; dev--) {
        if ((missing & (1u << (dev - 1u))) == 0u) {
            len += put_frame(&resp[len], dev, 0x0528u, &stat[dev - 1u], 1u);
        }
    }
    for (uint8_t dev = 16u; dev >= 1u; dev--) {
        if ((missing & (1u << (dev - 1u))) == 0u) {
            const uint8_t v[2] = {0x00, vcow2[dev - 1u]};
            len += put_frame(&resp[len], dev, 0x0548u, v, 2u);
        }
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
    /* With the chip's stop-on-fault (FLTSTOP_EN, spec #36), sampled at
       every BAL_GO. */
    const uint8_t balGo[] = {0x22};

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
    const uint8_t sinksOff[] = {0x00};
    const uint8_t gpioAdcOtut[] = {0x09, 0x09, 0x09, 0x09};
    const uint8_t thresholds[] = {0x24, 0x18, 0xF4};
    const uint8_t masks[] = {0x44, 0x1F};
    const uint8_t devConf[] = {0x54};
    const uint8_t owThr[] = {0x06};
    const uint8_t tsref[] = {0x01};
    const uint8_t continuous[] = {0x06};
    const uint8_t bridgeConf[] = {0x14};
    const uint8_t bridgeMask[] = {0xD8};

    bq_chain_init(&chain);
    uint32_t now = step_until(BQ_CHAIN_VERIFY);
    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_EQUAL_UINT(1u, plan.count);
    assert_read(0u, 0x0306u, 1u);
    (void)bq_chain_done(&chain, now, true, resp, address_readback(16u), &sweep);

    bq_chain_plan(&chain, now + STEP_MS, &plan);
    TEST_ASSERT_EQUAL_UINT(11u, plan.count);
    assert_write(0u, BQ_STACK_WRITE, 0u, 0x0003u, eightCells, 1u);  /* ACTIVE_CELL 8S */
    assert_write(1u, BQ_STACK_WRITE, 0u, 0x033Cu, sinksOff, 1u);    /* DIAG_COMP_CTRL3 */
    assert_write(2u, BQ_STACK_WRITE, 0u, 0x000Eu, gpioAdcOtut, 4u); /* GPIO1-8 ADC and OTUT */
    /* OV_THRESH 0x24 = 4175 + 2 x 25 = 4225 mV; UV_THRESH 0x18 = 1200 +
       24 x 50 = 2400 mV; OTUT_THRESH: UT 80 % (masked), OT 30 % of the
       5 V TSREF = 1500 mV, 61.7 C on the Enepaq table (60 C is 1510 mV;
       31 %, 1550 mV, is 55 C). */
    assert_write(3u, BQ_STACK_WRITE, 0u, 0x0009u, thresholds, 3u);
    /* FAULT_MSK1: UT and the ADC comparisons kept off NFAULT; FAULT_MSK2:
       the link's own faults kept off NFAULT. */
    assert_write(4u, BQ_STACK_WRITE, 0u, 0x0016u, masks, 2u);
    assert_write(5u, BQ_STACK_WRITE, 0u, 0x0002u, devConf, 1u);     /* FCOMM_EN, NFAULT_EN */
    assert_write(6u, BQ_STACK_WRITE, 0u, 0x033Bu, owThr, 1u);       /* OW_THR 2.3 V */
    assert_write(7u, BQ_STACK_WRITE, 0u, 0x030Au, tsref, 1u);       /* TSREF_EN: sensor bias */
    assert_write(8u, BQ_STACK_WRITE, 0u, 0x030Du, continuous, 1u);  /* MAIN_GO, continuous */
    assert_write(9u, BQ_SINGLE_WRITE, 0u, 0x2001u, bridgeConf, 1u); /* bridge FCOMM_EN, NFAULT_EN */
    assert_write(10u, BQ_SINGLE_WRITE, 0u, 0x2020u, bridgeMask, 1u); /* bridge FAULT_MSK */
    (void)bq_chain_done(&chain, now + STEP_MS, true, resp, 0u, &sweep);
    TEST_ASSERT_FALSE(bq_chain_running(&chain));
}

static void test_the_comparators_start_a_step_after_tsref_then_the_faults_are_cleared(void)
{
    const uint8_t go[] = {0x05, 0x05}; /* OVUV_CTRL, OTUT_CTRL: round robin, GO */
    const uint8_t stackReset[] = {0xFF, 0x7F};
    const uint8_t bridgeReset[] = {0xFF};

    bq_chain_init(&chain);
    uint32_t now = step_until(BQ_CHAIN_PROTECT);

    /* TSREF settles in 6 ms, and an OT comparator started before it flags
       every sensor (SLUSE81F 7.3.4.2): a step on, it has. */
    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_EQUAL_UINT(1u, plan.count);
    assert_write(0u, BQ_STACK_WRITE, 0u, 0x032Cu, go, 2u);
    (void)bq_chain_done(&chain, now, true, resp, 0u, &sweep);
    TEST_ASSERT_FALSE(bq_chain_running(&chain));

    /* What the wake and the start left latched: FAULT_RST1..2 on every
       stack device, then the bridge's FAULT_RST, a step after the
       comparators' first round. */
    bq_chain_plan(&chain, now + STEP_MS, &plan);
    TEST_ASSERT_EQUAL_UINT(2u, plan.count);
    assert_write(0u, BQ_STACK_WRITE, 0u, 0x0331u, stackReset, 2u);
    assert_write(1u, BQ_SINGLE_WRITE, 0u, 0x2030u, bridgeReset, 1u);
    (void)bq_chain_done(&chain, now + STEP_MS, true, resp, 0u, &sweep);

    TEST_ASSERT_TRUE(bq_chain_running(&chain));
}

static void test_a_failed_clear_is_a_failed_wake(void)
{
    bq_chain_init(&chain);
    uint32_t now = step_until(BQ_CHAIN_CLEAR);
    bq_chain_plan(&chain, now, &plan);
    (void)bq_chain_done(&chain, now, false, resp, 0u, &sweep);

    TEST_ASSERT_EQUAL(BQ_CHAIN_RETRY_WAIT, bq_chain_state(&chain));
}

static void test_a_running_chain_takes_one_sweep_per_step(void)
{
    bq_chain_init(&chain);
    uint32_t now = wake_from(0u) + STEP_MS;

    for (int i = 0; i < 5; i++, now += STEP_MS) {
        bq_chain_plan(&chain, now, &plan);
        TEST_ASSERT_FALSE(plan.wake_ping);
        TEST_ASSERT_EQUAL_UINT(2u, plan.count);
        assert_read(0u, BQ_SWEEP_REG, BQ_SWEEP_COUNT);
        TEST_ASSERT_FALSE(plan.cmd[0].append);
        assert_bridge_read(1u, 0x2100u, 1u); /* FAULT_SUMMARY */
        TEST_ASSERT_TRUE(bq_chain_done(&chain, now, true, resp, sweep_response(), &sweep));
        TEST_ASSERT_EQUAL_HEX16(0xFFFFu, sweep.devices_read);
        TEST_ASSERT_TRUE(sweep.bridge_read);
        TEST_ASSERT_EQUAL_HEX8(0x00u, sweep.bridge_fault);
    }
}

static void test_each_sweep_brings_back_the_bridges_fault_register(void)
{
    bq_chain_init(&chain);
    const uint32_t now = wake_from(0u) + STEP_MS;

    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_TRUE(bq_chain_done(&chain, now, true, resp, sweep_response_with(0x08u), &sweep));
    TEST_ASSERT_TRUE(sweep.bridge_read);
    TEST_ASSERT_EQUAL_HEX8(0x08u, sweep.bridge_fault); /* FAULT_COMM: a stack device's fault */
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
    TEST_ASSERT_FALSE(sweep.bridge_read); /* the plan stopped at the timeout */
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

/* ---- the open-wire check ------------------------------------------------ */

/* ADC_STAT2[DRDY_VCOW] on every device. */
static const uint8_t allDone[16] = {
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
};
static const uint8_t noOpen[16] = {0};

static bq_open_wire_t check;

/* Wakes the chain and plans the check on it. */
static void plan_check(void)
{
    bq_chain_init(&chain);
    (void)wake_from(0u);
    TEST_ASSERT_TRUE(bq_chain_plan_open_wire(&chain, &plan));
}

static void test_the_open_wire_check_is_planned_only_on_a_running_chain(void)
{
    bq_chain_init(&chain);
    TEST_ASSERT_FALSE(bq_chain_plan_open_wire(&chain, &plan));
    TEST_ASSERT_EQUAL_UINT(0u, plan.count);
    TEST_ASSERT_FALSE(plan.wake_ping);
}

static void test_the_open_wire_check_follows_ti(void)
{
    const uint8_t rstComp[] = {0x04};
    const uint8_t sinksOn[] = {0x10};
    const uint8_t compare[] = {0x15}; /* sinks on, VC OW check, COMP_ADC_GO */
    const uint8_t sinksOff[] = {0x00};

    plan_check();

    TEST_ASSERT_EQUAL_UINT(6u, plan.count);
    assert_write(0u, BQ_STACK_WRITE, 0u, 0x0331u, rstComp, 1u); /* the last result away */
    assert_write(1u, BQ_STACK_WRITE, 0u, 0x033Cu, sinksOn, 1u);
    TEST_ASSERT_EQUAL_UINT8(BQ_OPEN_WIRE_SETTLE_MS, plan.cmd[1].delay_ms);
    assert_write(2u, BQ_STACK_WRITE, 0u, 0x033Cu, compare, 1u);
    TEST_ASSERT_EQUAL_UINT8(1u, plan.cmd[2].delay_ms); /* a 192 us round robin, and more */
    assert_write(3u, BQ_STACK_WRITE, 0u, 0x033Cu, sinksOff, 1u);
    TEST_ASSERT_EQUAL_UINT8(0u, plan.cmd[3].delay_ms);
    assert_read(4u, 0x0528u, 1u); /* ADC_STAT2 */
    TEST_ASSERT_FALSE(plan.cmd[4].append);
    assert_read(5u, 0x0548u, 2u); /* FAULT_COMP_VCOW1..2 */
    TEST_ASSERT_TRUE(plan.cmd[5].append);
}

/* An open wire drains the 0.47 uF filter capacitors on both sides of its
   pin (the cell board's) at the sink's least 380 uA (SLUSE81F IOW_SINK):
   0.40 V/ms, so from a full 4.2 V cell to under 2.3 V in 4.7 ms. */
static void test_the_sinks_are_on_long_enough_to_drain_an_open_wire(void)
{
    TEST_ASSERT_EQUAL_UINT(5u, BQ_OPEN_WIRE_SETTLE_MS);
}

static void test_every_cell_above_the_threshold_on_every_device_is_a_pass(void)
{
    plan_check();
    bq_chain_open_wire_done(&chain, true, resp, open_wire_response(allDone, noOpen, 0u), &check);

    TEST_ASSERT_EQUAL(BQ_OPEN_WIRE_PASS, check.verdict);
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, check.devices_read);
    TEST_ASSERT_EQUAL_UINT8(0u, check.device);
    TEST_ASSERT_EQUAL_UINT16(0u, check.cell);
}

static void test_a_cell_under_the_threshold_fails_and_is_named(void)
{
    uint8_t vcow[16] = {0};
    vcow[5] = 0x24; /* device 6, cells 3 and 6 */

    plan_check();
    bq_chain_open_wire_done(&chain, true, resp, open_wire_response(allDone, vcow, 0u), &check);

    TEST_ASSERT_EQUAL(BQ_OPEN_WIRE_FAIL, check.verdict);
    TEST_ASSERT_EQUAL_UINT8(6u, check.device);
    TEST_ASSERT_EQUAL_UINT16(5u * 8u + 3u, check.cell);
}

/* A write has no answer, so a lost COMP_ADC_GO would leave every result
   clear: a device that did not finish has not shown its wires whole. */
static void test_a_device_that_did_not_finish_the_comparison_fails(void)
{
    uint8_t stat[16];
    memcpy(stat, allDone, sizeof stat);
    stat[11] = 0x00;

    plan_check();
    bq_chain_open_wire_done(&chain, true, resp, open_wire_response(stat, noOpen, 0u), &check);

    TEST_ASSERT_EQUAL(BQ_OPEN_WIRE_FAIL, check.verdict);
    TEST_ASSERT_EQUAL_UINT8(12u, check.device);
    TEST_ASSERT_EQUAL_UINT16(0u, check.cell);
}

/* A device that does not answer is a missing measurement, judged by the
   Sweep: the check neither passes nor fails. */
static void test_a_device_that_does_not_answer_leaves_the_check_unknown(void)
{
    plan_check();
    bq_chain_open_wire_done(&chain, true, resp, open_wire_response(allDone, noOpen, 0x0001u),
                            &check);

    TEST_ASSERT_EQUAL(BQ_OPEN_WIRE_UNKNOWN, check.verdict);
    TEST_ASSERT_EQUAL_HEX16(0xFFFEu, check.devices_read);
}

static void test_an_open_wire_seen_beside_a_silent_device_still_fails(void)
{
    uint8_t vcow[16] = {0};
    vcow[15] = 0x80; /* device 16, cell 8 */

    plan_check();
    bq_chain_open_wire_done(&chain, true, resp, open_wire_response(allDone, vcow, 0x0001u),
                            &check);

    TEST_ASSERT_EQUAL(BQ_OPEN_WIRE_FAIL, check.verdict);
    TEST_ASSERT_EQUAL_UINT16(128u, check.cell);
}

/* A check that stopped part way may have left the sinks on, which pull the
   bottom and top cells' readings: the next Sweep turns them off first. */
static void test_after_a_check_that_stopped_the_next_sweep_turns_the_sinks_off_first(void)
{
    const uint8_t sinksOff[] = {0x00};

    bq_chain_init(&chain);
    uint32_t now = wake_from(0u) + STEP_MS;
    (void)bq_chain_plan_open_wire(&chain, &plan);
    bq_chain_open_wire_done(&chain, false, resp, 0u, &check);
    TEST_ASSERT_EQUAL(BQ_OPEN_WIRE_UNKNOWN, check.verdict);

    bq_chain_plan(&chain, now, &plan);
    TEST_ASSERT_EQUAL_UINT(3u, plan.count);
    assert_write(0u, BQ_STACK_WRITE, 0u, 0x033Cu, sinksOff, 1u);
    assert_read(1u, BQ_SWEEP_REG, BQ_SWEEP_COUNT);
    TEST_ASSERT_TRUE(bq_chain_done(&chain, now, true, resp, sweep_response(), &sweep));
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, sweep.devices_read);

    /* Once a Sweep went whole, the sinks are surely off. */
    bq_chain_plan(&chain, now + STEP_MS, &plan);
    TEST_ASSERT_EQUAL_UINT(2u, plan.count);
}

/* The step with the check: its Sweep, then the check, inside the 20 ms
   step at worst (spec #36: the check runs after the step's work, in the
   same step). */
static void test_a_sweep_and_the_open_wire_check_fit_in_one_step(void)
{
    bq_plan_t sweepPlan;

    bq_chain_init(&chain);
    const uint32_t now = wake_from(0u) + STEP_MS;
    bq_chain_plan(&chain, now, &sweepPlan);
    TEST_ASSERT_TRUE(bq_chain_plan_open_wire(&chain, &plan));

    TEST_ASSERT_TRUE(bq_plan_worst_us(&sweepPlan) + bq_plan_worst_us(&plan) < 20000u);
    TEST_ASSERT_TRUE(bq_plan_response_len(&sweepPlan) <= BQ_RESPONSE_MAX);
    TEST_ASSERT_TRUE(bq_plan_response_len(&plan) <= BQ_RESPONSE_MAX);
}

/* A delay of n ms is n whole ticks plus the one the wait starts in. */
static void test_a_delay_counts_in_the_worst_case(void)
{
    plan_check();
    /* 4 writes at 300 us; (5 + 1) and (1 + 1) ms of waits; ADC_STAT2's
       read 50 + 1310 us and the VCOW read 50 + 1470 us. */
    TEST_ASSERT_EQUAL_UINT32(1200u + 8000u + 1360u + 1520u, bq_plan_worst_us(&plan));
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
    RUN_TEST(test_the_comparators_start_a_step_after_tsref_then_the_faults_are_cleared);
    RUN_TEST(test_a_failed_clear_is_a_failed_wake);
    RUN_TEST(test_a_running_chain_takes_one_sweep_per_step);
    RUN_TEST(test_each_sweep_brings_back_the_bridges_fault_register);
    RUN_TEST(test_a_sweep_that_times_out_is_a_missing_measurement);
    RUN_TEST(test_a_chain_that_does_not_answer_is_woken_again_a_second_after_the_last_try);
    RUN_TEST(test_retries_go_on_for_ever);
    RUN_TEST(test_a_wrong_address_read_back_is_a_failed_wake);
    RUN_TEST(test_a_chain_that_stops_answering_for_a_second_is_woken_again);
    RUN_TEST(test_the_sweep_read_waits_its_tWAIT_READ_MAX);
    RUN_TEST(test_no_step_of_the_wake_or_the_sweep_can_outlast_its_budget);
    RUN_TEST(test_the_open_wire_check_is_planned_only_on_a_running_chain);
    RUN_TEST(test_the_open_wire_check_follows_ti);
    RUN_TEST(test_the_sinks_are_on_long_enough_to_drain_an_open_wire);
    RUN_TEST(test_every_cell_above_the_threshold_on_every_device_is_a_pass);
    RUN_TEST(test_a_cell_under_the_threshold_fails_and_is_named);
    RUN_TEST(test_a_device_that_did_not_finish_the_comparison_fails);
    RUN_TEST(test_a_device_that_does_not_answer_leaves_the_check_unknown);
    RUN_TEST(test_an_open_wire_seen_beside_a_silent_device_still_fails);
    RUN_TEST(test_after_a_check_that_stopped_the_next_sweep_turns_the_sinks_off_first);
    RUN_TEST(test_a_sweep_and_the_open_wire_check_fit_in_one_step);
    RUN_TEST(test_a_delay_counts_in_the_worst_case);
    return UNITY_END();
}
