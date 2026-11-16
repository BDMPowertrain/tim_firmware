#ifndef APP_INC_VBAT_H
#define APP_INC_VBAT_H

#include <stdbool.h>
#include <stdint.h>

/* Calibrates ADC1 once. Must run before the first vbat_sample() — see
   tasks_init(). False if the calibration failed. */
bool vbat_init(void);

/* Takes one reading of VBAT_ADC (PC3/ADC1_IN9). Housekeeping calls it every
   100 ms as the VCU's poll (issue 47); a failed conversion keeps the last
   reading. */
void vbat_sample(void);

/* Latest VBAT reading — raw 12-bit ADC count and millivolts (scaled
   through the board's confirmed 1/13 divider, R21=120k/R22=10k,
   verified against a schematic design note). Returns false (leaving
   both out-params untouched) until the first conversion completes.
   raw and *out_mv are read as two independent scalars, not atomically
   together — Housekeeping could in principle sample between the two
   reads, momentarily pairing one sample's raw count with the next
   sample's millivolts. Deliberately not worth a mutex for this: worst
   case is a one-sample (100 ms) skew on a value that doesn't change
   meaningfully within seconds, let alone milliseconds. */
bool vbat_get_reading(uint32_t *out_raw, uint32_t *out_mv);

#endif /* APP_INC_VBAT_H */
