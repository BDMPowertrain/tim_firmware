/*
 * Host tests for what the BMS control step sends on the Control bus
 * (App/bms/logic/Inc/bms_tx.h).
 *
 * Spec (issue 36, "Control step, tasks and interrupts"): "BMS send table
 * over a five-step cycle: BMS_Status and BMS_Power every step, plus one
 * of ..." can/control.dbc, BMS_Status: "Every sweep (20 ms), from the
 * first control step. E2E Profile 11, Data ID 0x0080." This skeleton sends
 * BMS_Status only; the rest of the table comes with the values it carries.
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
    e2e_p11_tx_t tx = {0};
    e2e_p11_rx_t rx = {0};
    struct can_control_bms_status_t signals = {0};
    signals.state = CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE;

    for (int i = 0; i < 20; i++) {
        uint8_t frame[CAN_CONTROL_BMS_STATUS_LENGTH];
        bms_tx_build_status(&signals, &tx, frame);

        TEST_ASSERT_EQUAL(E2E_P11_OK,
                          e2e_p11_check(&rx, 0x0080u, frame, sizeof(frame)));

        struct can_control_bms_status_t out;
        TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(&out, frame, sizeof(frame)));
        TEST_ASSERT_EQUAL_UINT8(0u, out.state); /* 0 is STARTUP in the DBC */
        TEST_ASSERT_EQUAL_UINT8(0u, out.bms_ok);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % 15), out.counter);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bms_status_is_due_on_every_step);
    RUN_TEST(test_a_startup_bms_status_says_startup_bms_ok_low_and_passes_its_e2e_check);
    return UNITY_END();
}
