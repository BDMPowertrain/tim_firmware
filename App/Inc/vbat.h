#ifndef APP_INC_VBAT_H
#define APP_INC_VBAT_H

#include <stdbool.h>
#include <stdint.h>

/* Calibrates ADC1 once, then starts a periodic software timer (not a
   dedicated task — Documentation/tim-rtos-architecture.md section 6,
   ladder step 6) that samples VBAT_ADC (PC3/ADC1_IN9) every 500 ms.
   Must run before any call to vbat_get_millivolts() — see
   tasks_init(). */
void vbat_init(void);

/* Latest VBAT reading — raw 12-bit ADC count and millivolts (scaled
   through the board's confirmed 1/13 divider, R21=120k/R22=10k,
   verified against a schematic design note). Returns false (leaving
   both out-params untouched) until the first conversion completes.
   raw and *out_mv are read as two independent scalars, not atomically
   together — a timer callback could in principle land between the two
   reads, momentarily pairing one sample's raw count with the next
   sample's millivolts. Deliberately not worth a mutex for this: worst
   case is a one-sample (500 ms) skew on a value that doesn't change
   meaningfully within seconds, let alone milliseconds. */
bool vbat_get_reading(uint32_t *out_raw, uint32_t *out_mv);

#endif /* APP_INC_VBAT_H */
