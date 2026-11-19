#include "bms_daq.h"

#include <string.h>

#include "can_daq.h"
#include "cell_temp.h"

/* The dump's frames per module: voltages A, voltages B, temperatures. */
#define DUMP_FRAMES_PER_MODULE 3u

/* Publishes per summary (200 ms) and per dump (1 s). */
#define SUMMARY_PUBLISHES 2u
#define DUMP_PUBLISHES    10u

/* BMS_CellTemps's mark for a sensor not read validly (can/daq.dbc). */
#define TEMP_INVALID INT16_MIN

/* The most each publish takes: half the summaries and the dumps of two
   modules (16 modules over 10 publishes). */
_Static_assert(BQ_SWEEP_DEVICES / SUMMARY_PUBLISHES +
                   DUMP_FRAMES_PER_MODULE * ((BQ_SWEEP_DEVICES + DUMP_PUBLISHES - 1u) /
                                             DUMP_PUBLISHES) == BMS_DAQ_FRAMES_MAX,
               "BMS_DAQ_FRAMES_MAX is one publish's worst");

void bms_daq_build_status(const daq_status_t *in, uint8_t *frame)
{
    const struct can_daq_bms_daq_status_t signals = {
        .overrun_seen = (in->overruns != 0u) ? 1u : 0u,
        .background_stalled = in->background_stalled ? 1u : 0u,
        .control_bus_state = daq_status_control_bus_state(in),
        .card_state = in->card_state,
        .events_dropped = in->events_dropped ? 1u : 0u,
        .allocation_failed = in->allocation_failed ? 1u : 0u,
        .records_dropped = in->records_dropped,
    };

    (void)can_daq_bms_daq_status_pack(frame, &signals, CAN_DAQ_BMS_DAQ_STATUS_LENGTH);
}

/* The module's readings as the frames carry them: a cell not read is 0 mV
   (bq_sweep.h), a sensor not read validly is TEMP_INVALID. */
typedef struct {
    uint16_t mv[BQ_SWEEP_CELLS_PER_DEVICE];
    int16_t  dc[BQ_SWEEP_CELLS_PER_DEVICE];
} module_readings_t;

static void module_readings(const bq_sweep_t *s, uint8_t module, module_readings_t *out)
{
    const bool read = (s->devices_read & (1u << module)) != 0u;

    for (uint8_t k = 0; k < BQ_SWEEP_CELLS_PER_DEVICE; k++) {
        const size_t i = (size_t)module * BQ_SWEEP_CELLS_PER_DEVICE + k;
        int16_t dc;
        out->mv[k] = read ? s->cell_mv[i] : 0u;
        out->dc[k] = (read && cell_temp_from_mv(s->temp_mv[i], &dc)) ? dc : TEMP_INVALID;
    }
}

/* The cell voltages' raw value: 0.1 mV (scale 0.0001 V in can/daq.dbc);
   above 6553 mV is held at the top. */
static uint16_t tenth_mv(uint16_t mv)
{
    return (mv > UINT16_MAX / 10u) ? UINT16_MAX : (uint16_t)(mv * 10u);
}

/* The summary's 8-bit temperature (MaxTemp and MinTemp in can/daq.dbc,
   scale 0.5, offset -40): 0.5 C above -40 C, rounded, held to the
   signal's range. */
static uint8_t summary_temp(int16_t dc)
{
    const int32_t raw = ((int32_t)dc + 400 + 2) / 5;
    if (raw < 0) {
        return 0u;
    }
    return (raw > 255) ? 255u : (uint8_t)raw;
}

static uint8_t *frame_init(can_frame_t *f, uint32_t id, uint8_t len)
{
    memset(f, 0, sizeof *f);
    f->id = id;
    f->len = len;
    return f->data;
}

/* Over the cells and sensors read validly; with none, that half is 0. */
static void build_summary(const module_readings_t *m, uint8_t module, can_frame_t *f)
{
    struct can_daq_bms_module_summary_00_t sum = {0};
    uint8_t hi = 0, lo = 0, hot = 0, cool = 0;
    bool haveCell = false, haveTemp = false;

    for (uint8_t k = 0; k < BQ_SWEEP_CELLS_PER_DEVICE; k++) {
        if (m->mv[k] != 0u) {
            if (!haveCell || m->mv[k] > m->mv[hi]) { hi = k; }
            if (!haveCell || m->mv[k] < m->mv[lo]) { lo = k; }
            haveCell = true;
        }
        if (m->dc[k] != TEMP_INVALID) {
            if (!haveTemp || m->dc[k] > m->dc[hot]) { hot = k; }
            if (!haveTemp || m->dc[k] < m->dc[cool]) { cool = k; }
            haveTemp = true;
        }
    }
    if (haveCell) {
        sum.max_cell_voltage = tenth_mv(m->mv[hi]);
        sum.min_cell_voltage = tenth_mv(m->mv[lo]);
        sum.max_cell_number = hi;
        sum.min_cell_number = lo;
    }
    if (haveTemp) {
        sum.max_temp = summary_temp(m->dc[hot]);
        sum.min_temp = summary_temp(m->dc[cool]);
        sum.max_temp_sensor = hot;
        sum.min_temp_sensor = cool;
    }
    uint8_t *d = frame_init(f, CAN_DAQ_BMS_MODULE_SUMMARY_00_FRAME_ID + module,
                       CAN_DAQ_BMS_MODULE_SUMMARY_00_LENGTH);
    (void)can_daq_bms_module_summary_00_pack(d, &sum, f->len);
}

/* The module's three dump frames into f[0..2]. */
static void build_dump(const module_readings_t *m, uint8_t module, uint8_t bleed, can_frame_t *f)
{
    const uint32_t offset = DUMP_FRAMES_PER_MODULE * (uint32_t)module;

    const struct can_daq_bms_cell_voltages_a_00_t a = {
        .cell0 = tenth_mv(m->mv[0]), .cell1 = tenth_mv(m->mv[1]),
        .cell2 = tenth_mv(m->mv[2]), .cell3 = tenth_mv(m->mv[3]),
    };
    uint8_t *d = frame_init(&f[0], CAN_DAQ_BMS_CELL_VOLTAGES_A_00_FRAME_ID + offset,
                       CAN_DAQ_BMS_CELL_VOLTAGES_A_00_LENGTH);
    (void)can_daq_bms_cell_voltages_a_00_pack(d, &a, f[0].len);

    const struct can_daq_bms_cell_voltages_b_00_t b = {
        .cell4 = tenth_mv(m->mv[4]), .cell5 = tenth_mv(m->mv[5]),
        .cell6 = tenth_mv(m->mv[6]), .cell7 = tenth_mv(m->mv[7]),
    };
    d = frame_init(&f[1], CAN_DAQ_BMS_CELL_VOLTAGES_B_00_FRAME_ID + offset,
              CAN_DAQ_BMS_CELL_VOLTAGES_B_00_LENGTH);
    (void)can_daq_bms_cell_voltages_b_00_pack(d, &b, f[1].len);

    const struct can_daq_bms_cell_temps_00_t t = {
        .temp0 = m->dc[0], .temp1 = m->dc[1], .temp2 = m->dc[2], .temp3 = m->dc[3],
        .temp4 = m->dc[4], .temp5 = m->dc[5], .temp6 = m->dc[6], .temp7 = m->dc[7],
        .bleed0 = (uint8_t)(bleed & 1u),        .bleed1 = (uint8_t)((bleed >> 1) & 1u),
        .bleed2 = (uint8_t)((bleed >> 2) & 1u), .bleed3 = (uint8_t)((bleed >> 3) & 1u),
        .bleed4 = (uint8_t)((bleed >> 4) & 1u), .bleed5 = (uint8_t)((bleed >> 5) & 1u),
        .bleed6 = (uint8_t)((bleed >> 6) & 1u), .bleed7 = (uint8_t)((bleed >> 7) & 1u),
    };
    d = frame_init(&f[2], CAN_DAQ_BMS_CELL_TEMPS_00_FRAME_ID + offset, CAN_DAQ_BMS_CELL_TEMPS_00_LENGTH);
    (void)can_daq_bms_cell_temps_00_pack(d, &t, f[2].len);
}

size_t bms_daq_build_frames(uint32_t publish, const bq_sweep_t *sweep,
                            const uint8_t bleeding[BQ_SWEEP_DEVICES],
                            can_frame_t *out, size_t max)
{
    size_t n = 0;
    module_readings_t m;

    for (uint8_t module = 0; module < BQ_SWEEP_DEVICES; module++) {
        const bool summary = (module % SUMMARY_PUBLISHES) == (publish % SUMMARY_PUBLISHES);
        const bool dump = (module % DUMP_PUBLISHES) == (publish % DUMP_PUBLISHES);
        if (!summary && !dump) {
            continue;
        }
        module_readings(sweep, module, &m);
        if (summary && n < max) {
            build_summary(&m, module, &out[n++]);
        }
        if (dump && max - n >= DUMP_FRAMES_PER_MODULE) {
            build_dump(&m, module, bleeding[module], &out[n]);
            n += DUMP_FRAMES_PER_MODULE;
        }
    }
    return n;
}
