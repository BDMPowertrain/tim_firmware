/*
 * Host tests for what the VCU control step sends on the Control bus
 * (App/vcu/logic/Inc/vcu_tx.h).
 *
 * Spec (issue 36, "Control step, tasks and interrupts"): "VCU send table:
 * 0x0C0 every step, VCU_Status every second step." can/control.dbc,
 * VCU_Status: "Every second control step (20 ms), from the first step.
 * E2E Profile 11, Data ID 0x0090." During STARTUP the VCU sends no 0x0C0
 * (spec, "Start-up and reset"), so this skeleton sends VCU_Status only.
 */
#include "can_control.h"
#include "e2e_p11.h"
#include "unity.h"
#include "vcu_tx.h"

void setUp(void) {}
void tearDown(void) {}

static void test_vcu_status_is_due_on_the_first_step_and_every_second_one(void)
{
    TEST_ASSERT_TRUE(vcu_tx_status_due(0u));
    TEST_ASSERT_FALSE(vcu_tx_status_due(1u));
    TEST_ASSERT_TRUE(vcu_tx_status_due(2u));
    TEST_ASSERT_FALSE(vcu_tx_status_due(3u));
    TEST_ASSERT_TRUE(vcu_tx_status_due(100u));
    TEST_ASSERT_FALSE(vcu_tx_status_due(0xFFFFFFFFu));
}

static void test_a_startup_vcu_status_says_startup_and_passes_its_e2e_check(void)
{
    e2e_p11_tx_t tx = {0};
    e2e_p11_rx_t rx = {0};
    struct can_control_vcu_status_t signals = {0};
    signals.state = CAN_CONTROL_VCU_STATUS_STATE_STARTUP_CHOICE;
    signals.startup_blocked = 1u;

    for (int i = 0; i < 20; i++) {
        uint8_t frame[CAN_CONTROL_VCU_STATUS_LENGTH];
        vcu_tx_build_status(&signals, &tx, frame);

        /* A receiver sees a good CRC and a counter one past the last, every
         * frame, through the counter's wrap at 14. */
        TEST_ASSERT_EQUAL(E2E_P11_OK,
                          e2e_p11_check(&rx, 0x0090u, frame, sizeof(frame)));

        struct can_control_vcu_status_t out;
        TEST_ASSERT_EQUAL_INT(0, can_control_vcu_status_unpack(&out, frame, sizeof(frame)));
        TEST_ASSERT_EQUAL_UINT8(0u, out.state); /* 0 is STARTUP in the DBC */
        TEST_ASSERT_EQUAL_UINT8(1u, out.startup_blocked);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i % 15), out.counter);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vcu_status_is_due_on_the_first_step_and_every_second_one);
    RUN_TEST(test_a_startup_vcu_status_says_startup_and_passes_its_e2e_check);
    return UNITY_END();
}
