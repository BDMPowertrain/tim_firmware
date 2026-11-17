#ifndef APP_VCU_INC_VCU_ADC_H
#define APP_VCU_INC_VCU_ADC_H

/* vcu_adc — the VCU's analog inputs (issue 55). TIM3 triggers ADC1 and
   ADC2 at 1 kHz; each converts its channels in one scan and DMA1 writes
   the results into a circular buffer here. No interrupt runs per sample:
   a reader averages the newest scans when it needs them (adc_window.h).

   ADC1: APPS1, BSE front, sensor supply 1, VBAT.
   ADC2: APPS2, BSE rear, sensor supply 2.
   (The scan order is the one in targets/vcu/tim.ioc.)

   Any task may read: the buffers are only ever read, and a reader never
   touches the scan the DMA is writing. */

#include <stdbool.h>
#include <stdint.h>

#include "vcu_sensors.h"

/* Scans averaged by a read: one control step's worth at 1 kHz. */
#define VCU_ADC_WINDOW_SCANS 10u

/* Calibrates both ADCs, starts their DMA and then TIM3, waits for the
   first scan and discards it (spec, issue 36: the first conversion after
   enabling is not used). Call once from tasks_init(); it blocks 1-2 ms,
   at most 5. False if anything did not start or the first scan never
   came: the image then stays in STARTUP. A reading with no sample is
   ADC_WINDOW_EMPTY, which the sensors report as failed. */
bool vcu_adc_start(void);

/* The newest averaged pedal, brake and supply counts. A channel with no
   sample is ADC_WINDOW_EMPTY. */
void vcu_adc_read_sensors(vcu_sensor_raw_t *raw);

/* Housekeeping's VCU poll: hands the newest averaged VBAT count to
   vbat.c. */
void vcu_adc_poll_vbat(void);

#endif /* APP_VCU_INC_VCU_ADC_H */
