/*
 * Host tests for the BMS's DAQ bus status frame
 * (App/bms/logic/Inc/bms_daq.h). can/daq.dbc, BMS_DaqStatus (0x280): the
 * same layout as VCU_DaqStatus, whose signals are tested one by one in
 * tests/host/vcu/test_vcu_daq.c. This checks the BMS frame carries them.
 */
#include <string.h>

#include "bms_daq.h"
#include "can_daq.h"
#include "daq_status.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_every_status_signal_reaches_the_bms_frame(void)
{
    daq_status_t in = {0};
    in.overruns = 3u;
    in.background_stalled = true;
    in.control_bus_off = true;
    in.card_state = CAN_DAQ_BMS_DAQ_STATUS_CARD_STATE_LOGGING_CHOICE;
    in.records_dropped = 7u;

    uint8_t frame[CAN_DAQ_BMS_DAQ_STATUS_LENGTH];
    memset(frame, 0xA5, sizeof(frame));
    bms_daq_build_status(&in, frame);

    /* Worked from the DBC: byte 0 = OverrunSeen 1 | BackgroundStalled 1<<1
     * | ControlBusState BUS_OFF 2<<2 | CardState LOGGING 1<<4 = 0x1B;
     * bytes 4-7 RecordsDropped 7, little-endian. */
    const uint8_t expected[CAN_DAQ_BMS_DAQ_STATUS_LENGTH] = {
        0x1Bu, 0x00u, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u, 0x00u,
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(frame));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_status_signal_reaches_the_bms_frame);
    return UNITY_END();
}
