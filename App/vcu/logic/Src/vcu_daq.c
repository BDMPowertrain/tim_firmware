#include "vcu_daq.h"

#include "can_daq.h"

void vcu_daq_build_status(const daq_status_t *in, uint8_t *frame)
{
    const struct can_daq_vcu_daq_status_t signals = {
        .overrun_seen = (in->overruns != 0u) ? 1u : 0u,
        .background_stalled = in->background_stalled ? 1u : 0u,
        .control_bus_state = daq_status_control_bus_state(in),
        .card_state = in->card_state,
        .events_dropped = in->events_dropped ? 1u : 0u,
        .allocation_failed = in->allocation_failed ? 1u : 0u,
        .records_dropped = in->records_dropped,
    };

    (void)can_daq_vcu_daq_status_pack(frame, &signals, CAN_DAQ_VCU_DAQ_STATUS_LENGTH);
}
