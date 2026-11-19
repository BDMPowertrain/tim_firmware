/*
 * Host tests for what the BMS control step sends on the Control bus
 * (App/bms/logic/Inc/bms_tx.h).
 *
 * Spec (issue 36, "Control step, tasks and interrupts"): "BMS send table
 * over a five-step cycle: BMS_Status and BMS_Power every step, plus one
 * of ..." can/control.dbc, BMS_Status: "Every sweep (20 ms), from the
 * first control step. E2E Profile 11, Data ID 0x0080."
 *
 * Issue 66: "Five-step send cycle: BMS_Status and BMS_Power every step,
 * plus one of BMS_Limits, 0x202, 0x60D, BMS_PackVoltages, BMS_PackTemps;
 * never more than three due on a step." "0x60D reaches the bus at least
 * every 250 ms" (EM manual p.10). Three is the controller's send slots
 * (ADR 0004): a frame never waits for one.
 */
#include "bms_tx.h"
#include "can_control.h"
#include "e2e_p11.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_bms_status_is_due_on_every_step(void)
{
    TEST_ASSERT_TRUE(bms_tx_status_due(0u));
    TEST_ASSERT_TRUE(bms_tx_status_due(1u));
    TEST_ASSERT_TRUE(bms_tx_status_due(4u));
    TEST_ASSERT_TRUE(bms_tx_status_due(0xFFFFFFFFu));
}

static void test_a_startup_bms_status_says_startup_bms_ok_low_and_passes_its_e2e_check(void)
{
    bms_tx_e2e_t tx = {0};
    e2e_p11_rx_t rx = {0};
    struct can_control_bms_status_t signals = {0};
    signals.state = CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE;

    for (int i = 0; i < 20; i++) {
        bms_tx_frame_t f;
        bms_tx_build_status(&signals, &tx, &f);

        TEST_ASSERT_EQUAL_UINT32(0x080u, f.id);
        TEST_ASSERT_FALSE(f.extended);
        TEST_ASSERT_EQUAL_UINT8(8u, f.len);
        TEST_ASSERT_EQUAL(E2E_P11_OK, e2e_p11_check(&rx, 0x0080u, f.data, f.len));

        struct can_control_bms_status_t out;
        TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(&out, f.data, f.len));
        TEST_ASSERT_EQUAL_UINT8(0u, out.state); /* 0 is STARTUP in the DBC */
        TEST_ASSERT_EQUAL_UINT8(0u, out.bms_ok);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % 15), out.counter);
    }
}

/* Issue 67: the charger request takes the 0x202 slot (step 1 of the five)
   on one step in fifty: step 1 of each second. */
static void test_the_charger_request_is_due_in_the_0x202_slot_once_in_fifty_steps(void)
{
    unsigned due = 0u;
    for (uint32_t n = 0u; n < 500u; n++) {
        if (bms_tx_charger_request_due(n)) {
            due++;
            TEST_ASSERT_EQUAL_UINT32(1u, n % 50u);
            TEST_ASSERT_EQUAL(BMS_TX_SLOW_INVERTER_LIMITS, bms_tx_slow_due(n));
        }
    }
    TEST_ASSERT_EQUAL_UINT(10u, due);
}

/* Issue 68: BMS_Balancing takes the 0x202 slot on step 26 of each second
   (can/control.dbc), never the charger request's. */
static void test_the_balancing_frame_is_due_in_the_0x202_slot_on_step_26_of_fifty(void)
{
    unsigned due = 0u;
    for (uint32_t n = 0u; n < 500u; n++) {
        if (bms_tx_balancing_due(n)) {
            due++;
            TEST_ASSERT_EQUAL_UINT32(26u, n % 50u);
            TEST_ASSERT_EQUAL(BMS_TX_SLOW_INVERTER_LIMITS, bms_tx_slow_due(n));
            TEST_ASSERT_FALSE(bms_tx_charger_request_due(n));
        }
    }
    TEST_ASSERT_EQUAL_UINT(10u, due);
}

/* can/control.dbc BMS_Balancing (0x600): State bits 0-3, Reason byte 1,
   CellSpread bytes 2-3 in mV, little-endian. */
static void test_the_balancing_frame_carries_state_reason_and_spread(void)
{
    const uint8_t want[8] = { 3, 5, 0x34, 0x12, 0, 0, 0, 0 };
    bms_tx_frame_t f;

    bms_tx_build_balancing(3u, 5u, 0x1234u, &f);
    TEST_ASSERT_EQUAL_HEX32(0x600u, f.id);
    TEST_ASSERT_FALSE(f.extended);
    TEST_ASSERT_EQUAL_UINT8(8u, f.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.data, 8u);
}

/* Elcon CAN Specification p.2, Message 1: 29-bit 0x1806E5F4, big-endian;
   BYTE1-2 voltage 0.1 V, BYTE3-4 current 0.1 A, BYTE5 control. */
static void test_the_charger_request_is_the_elcon_message_1_layout(void)
{
    const struct can_control_chg_request_t r = {
        .max_charge_voltage = 5248u, /* 524.8 V */
        .max_charge_current = 20u,   /* 2.0 A */
        .control = 0u,
    };
    const uint8_t want[8] = { 0x14, 0x80, 0x00, 0x14, 0x00, 0, 0, 0 };
    bms_tx_frame_t f;

    bms_tx_build_charger_request(&r, &f);
    TEST_ASSERT_EQUAL_HEX32(0x1806E5F4u, f.id);
    TEST_ASSERT_TRUE(f.extended);
    TEST_ASSERT_EQUAL_UINT8(8u, f.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.data, 8u);
}

static unsigned due_on(uint32_t n)
{
    return (bms_tx_status_due(n) ? 1u : 0u) + (bms_tx_power_due(n) ? 1u : 0u) +
           (bms_tx_slow_due(n) != BMS_TX_SLOW_NONE ? 1u : 0u);
}

static void test_bms_power_is_due_on_every_step(void)
{
    TEST_ASSERT_TRUE(bms_tx_power_due(0u));
    TEST_ASSERT_TRUE(bms_tx_power_due(1u));
    TEST_ASSERT_TRUE(bms_tx_power_due(0xFFFFFFFFu));
}

/* Over every five steps each slower frame goes exactly once, and no step
   has more than three frames due. */
static void test_each_slow_frame_goes_once_in_five_steps_and_never_more_than_three_a_step(void)
{
    for (uint32_t cycle = 0; cycle < 4u; cycle++) {
        unsigned seen[BMS_TX_SLOW_COUNT] = {0};
        for (uint32_t i = 0; i < 5u; i++) {
            const uint32_t n = cycle * 5u + i;
            const bms_tx_slow_t slow = bms_tx_slow_due(n);
            TEST_ASSERT_NOT_EQUAL(BMS_TX_SLOW_NONE, slow);
            TEST_ASSERT_LESS_THAN(BMS_TX_SLOW_COUNT, slow);
            seen[slow]++;
            TEST_ASSERT_LESS_OR_EQUAL_UINT(3u, due_on(n));
            TEST_ASSERT_LESS_OR_EQUAL_UINT(BMS_TX_FRAMES_MAX, due_on(n));
        }
        for (unsigned f = 0; f < (unsigned)BMS_TX_SLOW_COUNT; f++) {
            TEST_ASSERT_EQUAL_UINT(1u, seen[f]);
        }
    }
}

/* At the step count's wrap (after 994 days) one slot repeats; every slow
   frame still goes within six steps, and 0x60D inside its 250 ms. */
static void test_across_the_step_counts_wrap_no_frame_waits_more_than_six_steps(void)
{
    unsigned last[BMS_TX_SLOW_COUNT] = {0};
    unsigned k = 0u;
    for (uint32_t n = 0xFFFFFFFFu - 20u; k < 40u; n++, k++) {
        last[bms_tx_slow_due(n)] = k;
        if (k >= 6u) {
            for (unsigned f = 0; f < (unsigned)BMS_TX_SLOW_COUNT; f++) {
                TEST_ASSERT_LESS_OR_EQUAL_UINT(6u, k - last[f]);
            }
        }
    }
}

/* 0x60D every fifth step, 100 ms: inside the meter's 250 ms. */
static void test_the_energy_meter_temperatures_go_every_100_ms(void)
{
    uint32_t lastMs = 0u;
    bool sent = false;
    for (uint32_t n = 0; n < 100u; n++) {
        if (bms_tx_slow_due(n) == BMS_TX_SLOW_EM_CELL_TEMPS) {
            const uint32_t ms = n * 20u;
            if (sent) {
                TEST_ASSERT_EQUAL_UINT32(100u, ms - lastMs);
            } else {
                TEST_ASSERT_LESS_OR_EQUAL_UINT32(250u, ms);
            }
            lastMs = ms;
            sent = true;
        }
    }
    TEST_ASSERT_TRUE(sent);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_charger_request_is_due_in_the_0x202_slot_once_in_fifty_steps);
    RUN_TEST(test_the_charger_request_is_the_elcon_message_1_layout);
    RUN_TEST(test_the_balancing_frame_is_due_in_the_0x202_slot_on_step_26_of_fifty);
    RUN_TEST(test_the_balancing_frame_carries_state_reason_and_spread);
    RUN_TEST(test_bms_status_is_due_on_every_step);
    RUN_TEST(test_a_startup_bms_status_says_startup_bms_ok_low_and_passes_its_e2e_check);
    RUN_TEST(test_bms_power_is_due_on_every_step);
    RUN_TEST(test_each_slow_frame_goes_once_in_five_steps_and_never_more_than_three_a_step);
    RUN_TEST(test_across_the_step_counts_wrap_no_frame_waits_more_than_six_steps);
    RUN_TEST(test_the_energy_meter_temperatures_go_every_100_ms);
    return UNITY_END();
}
