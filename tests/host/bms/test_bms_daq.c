/*
 * Host tests for what the BMS sends on the DAQ bus
 * (App/bms/logic/Inc/bms_daq.h): BMS_DaqStatus, the module summaries and
 * the per-cell dump (can/daq.dbc). The frames are read back with the
 * generated unpack code, so the DBC's layout is what is checked.
 */
#include <string.h>

#include "bms_daq.h"
#include "can_daq.h"
#include "daq_status.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static bq_sweep_t sweep;
static uint8_t bleeding[BQ_SWEEP_DEVICES];
static can_frame_t frames[BMS_DAQ_FRAMES_MAX];

/* Every device read, every cell 3.700 V, every sensor 1860 mV (25.0 C,
   Enepaq Table 5). */
static void even_pack(void)
{
    memset(&sweep, 0, sizeof sweep);
    memset(bleeding, 0, sizeof bleeding);
    sweep.devices_read = BQ_SWEEP_ALL_DEVICES;
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = 3700u;
        sweep.temp_mv[i] = 1860u;
    }
}

/* The frame with this ID among the n built, or NULL. */
static const can_frame_t *find(size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (frames[i].id == id) {
            return &frames[i];
        }
    }
    return NULL;
}

static size_t build(uint32_t publish)
{
    memset(frames, 0xA5, sizeof frames);
    return bms_daq_build_frames(publish, &sweep, bleeding, frames, BMS_DAQ_FRAMES_MAX);
}

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

static void test_a_module_summary_holds_its_extremes_and_where_they_are(void)
{
    even_pack();
    /* Module 2: cell 5 highest, cell 1 lowest; sensor 6 hottest (1550 mV,
       55.0 C), sensor 3 coolest (2170 mV, 0.0 C). */
    sweep.cell_mv[2 * 8 + 5] = 4012u;
    sweep.cell_mv[2 * 8 + 1] = 3456u;
    sweep.temp_mv[2 * 8 + 6] = 1550u;
    sweep.temp_mv[2 * 8 + 3] = 2170u;

    const size_t n = build(0u);
    const can_frame_t *f = find(n, CAN_DAQ_BMS_MODULE_SUMMARY_00_FRAME_ID + 2u);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_BMS_MODULE_SUMMARY_00_LENGTH, f->len);
    TEST_ASSERT_FALSE(f->is_extended);

    struct can_daq_bms_module_summary_00_t s;
    TEST_ASSERT_EQUAL_INT(0, can_daq_bms_module_summary_00_unpack(&s, f->data, f->len));
    TEST_ASSERT_EQUAL_UINT16(40120u, s.max_cell_voltage); /* 0.1 mV */
    TEST_ASSERT_EQUAL_UINT16(34560u, s.min_cell_voltage);
    TEST_ASSERT_EQUAL_UINT8(5u, s.max_cell_number);
    TEST_ASSERT_EQUAL_UINT8(1u, s.min_cell_number);
    /* 0.5 C above -40 C: 55.0 C is 190, 0.0 C is 80. */
    TEST_ASSERT_EQUAL_UINT8(190u, s.max_temp);
    TEST_ASSERT_EQUAL_UINT8(80u, s.min_temp);
    TEST_ASSERT_EQUAL_UINT8(6u, s.max_temp_sensor);
    TEST_ASSERT_EQUAL_UINT8(3u, s.min_temp_sensor);
}

static void test_each_module_summary_goes_once_in_every_two_publishes(void)
{
    even_pack();
    for (uint32_t first = 0; first < 4u; first++) {
        uint8_t seen[BQ_SWEEP_DEVICES] = {0};
        for (uint32_t p = first; p < first + 2u; p++) {
            const size_t n = build(p);
            for (size_t i = 0; i < n; i++) {
                const uint32_t m = frames[i].id - CAN_DAQ_BMS_MODULE_SUMMARY_00_FRAME_ID;
                if (m < BQ_SWEEP_DEVICES) {
                    seen[m]++;
                }
            }
        }
        for (size_t m = 0; m < BQ_SWEEP_DEVICES; m++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, seen[m], "a summary every 200 ms");
        }
    }
}

static void test_a_module_not_read_is_summarised_as_zeros(void)
{
    even_pack();
    sweep.devices_read = (uint16_t)(BQ_SWEEP_ALL_DEVICES & ~(1u << 4));

    const size_t n = build(0u);
    const can_frame_t *f = find(n, CAN_DAQ_BMS_MODULE_SUMMARY_00_FRAME_ID + 4u);
    TEST_ASSERT_NOT_NULL(f);
    const uint8_t zeros[CAN_DAQ_BMS_MODULE_SUMMARY_00_LENGTH] = {0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, f->data, sizeof zeros);
}

static void test_a_summary_leaves_out_a_sensor_outside_its_range(void)
{
    even_pack();
    /* Module 1, sensor 2 open (above 2440 mV): not the coolest. */
    sweep.temp_mv[1 * 8 + 2] = 3000u;
    sweep.temp_mv[1 * 8 + 7] = 1920u; /* 20.0 C */

    const size_t n = build(1u);
    const can_frame_t *f = find(n, CAN_DAQ_BMS_MODULE_SUMMARY_00_FRAME_ID + 1u);
    TEST_ASSERT_NOT_NULL(f);
    struct can_daq_bms_module_summary_00_t s;
    (void)can_daq_bms_module_summary_00_unpack(&s, f->data, f->len);
    TEST_ASSERT_EQUAL_UINT8(120u, s.min_temp); /* 20.0 C */
    TEST_ASSERT_EQUAL_UINT8(7u, s.min_temp_sensor);
    TEST_ASSERT_EQUAL_UINT8(130u, s.max_temp); /* 25.0 C */
}

/* The publish, 0 to 9, on which module m's dump goes; 99 if none. */
static uint32_t dump_publish(uint8_t m)
{
    for (uint32_t p = 0; p < 10u; p++) {
        const size_t n = build(p);
        if (find(n, CAN_DAQ_BMS_CELL_TEMPS_00_FRAME_ID + 3u * m) != NULL) {
            return p;
        }
    }
    return 99u;
}

static void test_each_module_dump_goes_once_a_second_in_three_frames(void)
{
    even_pack();
    for (uint32_t first = 0; first < 10u; first++) {
        uint8_t seen[3u * BQ_SWEEP_DEVICES] = {0};
        for (uint32_t p = first; p < first + 10u; p++) {
            const size_t n = build(p);
            TEST_ASSERT_LESS_OR_EQUAL_size_t(BMS_DAQ_FRAMES_MAX, n);
            for (size_t i = 0; i < n; i++) {
                const uint32_t k = frames[i].id - CAN_DAQ_BMS_CELL_VOLTAGES_A_00_FRAME_ID;
                if (frames[i].id >= CAN_DAQ_BMS_CELL_VOLTAGES_A_00_FRAME_ID &&
                    k < 3u * BQ_SWEEP_DEVICES) {
                    seen[k]++;
                }
            }
        }
        for (size_t k = 0; k < 3u * BQ_SWEEP_DEVICES; k++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, seen[k], "each dump frame every 1 s");
        }
    }
}

static void test_a_dump_holds_every_cell_voltage_of_its_module(void)
{
    even_pack();
    for (uint8_t k = 0; k < 8u; k++) {
        sweep.cell_mv[9 * 8 + k] = (uint16_t)(3600u + k);
    }
    const size_t n = build(dump_publish(9u));
    const can_frame_t *a = find(n, CAN_DAQ_BMS_CELL_VOLTAGES_A_00_FRAME_ID + 27u);
    const can_frame_t *b = find(n, CAN_DAQ_BMS_CELL_VOLTAGES_B_00_FRAME_ID + 27u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_BMS_CELL_VOLTAGES_A_00_LENGTH, a->len);
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_BMS_CELL_VOLTAGES_B_00_LENGTH, b->len);

    struct can_daq_bms_cell_voltages_a_00_t va;
    struct can_daq_bms_cell_voltages_b_00_t vb;
    (void)can_daq_bms_cell_voltages_a_00_unpack(&va, a->data, a->len);
    (void)can_daq_bms_cell_voltages_b_00_unpack(&vb, b->data, b->len);
    TEST_ASSERT_EQUAL_UINT16(36000u, va.cell0);
    TEST_ASSERT_EQUAL_UINT16(36010u, va.cell1);
    TEST_ASSERT_EQUAL_UINT16(36020u, va.cell2);
    TEST_ASSERT_EQUAL_UINT16(36030u, va.cell3);
    TEST_ASSERT_EQUAL_UINT16(36040u, vb.cell4);
    TEST_ASSERT_EQUAL_UINT16(36070u, vb.cell7);
}

static void test_a_dump_holds_temperatures_and_bleed_flags(void)
{
    even_pack();
    sweep.temp_mv[15 * 8 + 0] = 1550u; /* 55.0 C */
    sweep.temp_mv[15 * 8 + 4] = 1000u; /* shorted: outside 1300-2440 mV */
    bleeding[15] = 0x82u;              /* cells 1 and 7 */

    const size_t n = build(dump_publish(15u));
    const can_frame_t *f = find(n, CAN_DAQ_BMS_CELL_TEMPS_00_FRAME_ID + 45u);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_BMS_CELL_TEMPS_00_LENGTH, f->len);

    struct can_daq_bms_cell_temps_00_t t;
    (void)can_daq_bms_cell_temps_00_unpack(&t, f->data, f->len);
    TEST_ASSERT_EQUAL_INT16(550, t.temp0);
    TEST_ASSERT_EQUAL_INT16(250, t.temp1);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, t.temp4); /* the invalid mark */
    TEST_ASSERT_EQUAL_UINT8(0u, t.bleed0);
    TEST_ASSERT_EQUAL_UINT8(1u, t.bleed1);
    TEST_ASSERT_EQUAL_UINT8(0u, t.bleed2);
    TEST_ASSERT_EQUAL_UINT8(1u, t.bleed7);
    /* Bytes 17-19 spare. */
    TEST_ASSERT_EQUAL_UINT8(0u, f->data[17]);
    TEST_ASSERT_EQUAL_UINT8(0u, f->data[19]);
}

static void test_a_dump_of_a_module_not_read_holds_no_reading(void)
{
    even_pack();
    sweep.devices_read = (uint16_t)(BQ_SWEEP_ALL_DEVICES & ~(1u << 3));

    const size_t n = build(dump_publish(3u));
    const can_frame_t *a = find(n, CAN_DAQ_BMS_CELL_VOLTAGES_A_00_FRAME_ID + 9u);
    const can_frame_t *t = find(n, CAN_DAQ_BMS_CELL_TEMPS_00_FRAME_ID + 9u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(t);
    struct can_daq_bms_cell_voltages_a_00_t va;
    struct can_daq_bms_cell_temps_00_t tt;
    (void)can_daq_bms_cell_voltages_a_00_unpack(&va, a->data, a->len);
    (void)can_daq_bms_cell_temps_00_unpack(&tt, t->data, t->len);
    TEST_ASSERT_EQUAL_UINT16(0u, va.cell0);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, tt.temp0);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, tt.temp7);
}

static void test_no_publish_gives_more_frames_than_asked_for(void)
{
    even_pack();
    for (uint32_t p = 0; p < 10u; p++) {
        TEST_ASSERT_LESS_OR_EQUAL_size_t(5u, bms_daq_build_frames(p, &sweep, bleeding, frames, 5u));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_status_signal_reaches_the_bms_frame);
    RUN_TEST(test_a_module_summary_holds_its_extremes_and_where_they_are);
    RUN_TEST(test_each_module_summary_goes_once_in_every_two_publishes);
    RUN_TEST(test_a_module_not_read_is_summarised_as_zeros);
    RUN_TEST(test_a_summary_leaves_out_a_sensor_outside_its_range);
    RUN_TEST(test_each_module_dump_goes_once_a_second_in_three_frames);
    RUN_TEST(test_a_dump_holds_every_cell_voltage_of_its_module);
    RUN_TEST(test_a_dump_holds_temperatures_and_bleed_flags);
    RUN_TEST(test_a_dump_of_a_module_not_read_holds_no_reading);
    RUN_TEST(test_no_publish_gives_more_frames_than_asked_for);
    return UNITY_END();
}
