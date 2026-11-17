/*
 * The ADC window: the newest samples in a circular DMA buffer (issue 36,
 * 55). A timer triggers the ADC, which converts its channels in order (one
 * scan) and the DMA writes each result into the buffer, scan after scan,
 * wrapping at the end. No interrupt runs per sample; a reader takes what
 * it needs from the buffer when it needs it.
 *
 * The buffer is scans x channels results, scan-major: channel c of scan s
 * is buf[s * channels + c]. Where the DMA is comes from its remaining
 * count (CNDTR), which runs from scans x channels down to 1 and reloads.
 * The scan the DMA is writing is never read. Every other slot was written
 * at least scans - 1 whole scans before the next write to it, so a reader
 * averaging fewer than that many scans finishes long before the DMA comes
 * round.
 *
 * A slot holding ADC_WINDOW_EMPTY is skipped: the buffer starts filled
 * with it, so a reader before the first scans sees none, and the first
 * scan, which the erratum says to discard, is set back to it.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef ADC_WINDOW_H
#define ADC_WINDOW_H

#include <stdint.h>

/* Never a 12-bit result. */
#define ADC_WINDOW_EMPTY 0xFFFFu

typedef struct {
    const volatile uint16_t *buf; /* scans * channels, written by the DMA */
    uint16_t                 scans;
    uint8_t                  channels;
} adc_window_t;

/* Averages each channel over the newest `count` complete scans before the
   DMA position given by `remaining`, skipping ADC_WINDOW_EMPTY slots.
   out[c] is the rounded mean, or ADC_WINDOW_EMPTY if channel c had no
   sample. count is clamped to scans - 1. */
void adc_window_average(const adc_window_t *w, uint16_t remaining, uint16_t count,
                        uint16_t *out);

#endif /* ADC_WINDOW_H */
