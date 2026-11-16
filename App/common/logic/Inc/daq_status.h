/*
 * What an image's DAQ bus status frame reports (issue 36, "status bits
 * carrying the warnings"): warnings that are not faults, sent every 100 ms
 * by the DAQ publisher from its copy of the Control step's published
 * snapshot. VCU_DaqStatus and BMS_DaqStatus share one layout (can/daq.dbc);
 * each image packs its own frame from this (vcu_daq.h, bms_daq.h).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef DAQ_STATUS_H
#define DAQ_STATUS_H

#include <stdbool.h>
#include <stdint.h>

/* Zero-initialise: nothing to report. */
typedef struct {
    uint32_t overruns;           /* since power-on */
    bool     background_stalled; /* Housekeeping has not run for 1 s */
    bool     control_bus_off;
    bool     control_bus_latched;
    bool     control_bus_error_passive;
    uint8_t  card_state;         /* the DBC's CardState choice */
    bool     events_dropped;
    bool     allocation_failed;
    uint32_t records_dropped;
} daq_status_t;

/* The DBC's ControlBusState choice for the Control bus's flags: the worst
 * of BUS_OFF_LATCHED, BUS_OFF, ERROR_PASSIVE, else ERROR_ACTIVE. */
uint8_t daq_status_control_bus_state(const daq_status_t *in);

#endif /* DAQ_STATUS_H */
