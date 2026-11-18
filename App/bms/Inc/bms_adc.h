#ifndef APP_BMS_INC_BMS_ADC_H
#define APP_BMS_INC_BMS_ADC_H

/* bms_adc — the BMS's analog inputs (issue 62; pin map, issue 19). TIM3
   triggers ADC1 at 1 kHz; it converts one scan of five channels and DMA1
   writes the results into a circular buffer here. No interrupt runs per
   sample: a reader averages the newest scans when it needs them
   (adc_window.h).

   Scan order (targets/bms/tim.ioc): board temperature (PA0), the two pack
   current sensor inputs reserved for #29 (PA1, PA3), VBAT (PC3), the
   internal reference. Board temperature and VBAT are reported only; the
   current inputs are sampled but not interpreted. Nothing here ever drops
   BMS_OK.

   Any task may read: the buffer is only ever read, and a reader never
   touches the scan the DMA is writing. */

#include <stdbool.h>
#include <stdint.h>

/* Scans averaged by a read: half a Sweep at 1 kHz. */
#define BMS_ADC_WINDOW_SCANS 10u

/* Raw 12-bit counts, each ADC_WINDOW_EMPTY if it has no sample. */
typedef struct {
    uint16_t board_temp;
    uint16_t current[2];
    uint16_t vbat;
    uint16_t vrefint;
} bms_adc_counts_t;

/* Calibrates ADC1, starts its DMA and then TIM3, waits for the first scan
   and discards it (spec, issue 36). Call once from tasks_init(); it blocks
   1-2 ms, at most 5. False if anything did not start. */
bool bms_adc_start(void);

/* The newest averaged counts. */
void bms_adc_read(bms_adc_counts_t *out);

/* Housekeeping's BMS poll: hands the newest VBAT count to vbat.c, as on
   the VCU (#47). */
void bms_adc_poll_vbat(void);

#endif /* APP_BMS_INC_BMS_ADC_H */
