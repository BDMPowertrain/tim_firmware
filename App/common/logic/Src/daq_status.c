#include "daq_status.h"

#include "can_daq.h"

/* The choice values are the same in both images' frames (can/daq.dbc). */
uint8_t daq_status_control_bus_state(const daq_status_t *in)
{
    if (in->control_bus_latched) {
        return CAN_DAQ_VCU_DAQ_STATUS_CONTROL_BUS_STATE_BUS_OFF_LATCHED_CHOICE;
    }
    if (in->control_bus_off) {
        return CAN_DAQ_VCU_DAQ_STATUS_CONTROL_BUS_STATE_BUS_OFF_CHOICE;
    }
    if (in->control_bus_error_passive) {
        return CAN_DAQ_VCU_DAQ_STATUS_CONTROL_BUS_STATE_ERROR_PASSIVE_CHOICE;
    }
    return CAN_DAQ_VCU_DAQ_STATUS_CONTROL_BUS_STATE_ERROR_ACTIVE_CHOICE;
}
