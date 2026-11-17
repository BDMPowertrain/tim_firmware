#ifndef APP_INC_VBAT_H
#define APP_INC_VBAT_H

#include <stdbool.h>
#include <stdint.h>

/* Records a VBAT_ADC (PC3/ADC1_IN9) count. The image's ADC driver hands
   it over: on the VCU, Housekeeping's 100 ms poll averages the newest
   VBAT samples from the timer-triggered ADC1 scan (vcu_adc.h). */
void vbat_record(uint16_t raw);

/* Latest VBAT reading — raw 12-bit ADC count and millivolts (scaled
   through the board's confirmed 1/13 divider, R21=120k/R22=10k,
   verified against a schematic design note). Returns false (leaving
   both out-params untouched) until the first count is recorded.
   raw and *out_mv are read as two independent scalars, not atomically
   together — Housekeeping could in principle record between the two
   reads, momentarily pairing one sample's raw count with the next
   sample's millivolts. Deliberately not worth a mutex for this: worst
   case is a one-sample (100 ms) skew on a value that doesn't change
   meaningfully within seconds, let alone milliseconds. */
bool vbat_get_reading(uint32_t *out_raw, uint32_t *out_mv);

#endif /* APP_INC_VBAT_H */
