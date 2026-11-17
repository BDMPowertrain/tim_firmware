/* adc_window.c — see adc_window.h. */

#include "adc_window.h"

void adc_window_average(const adc_window_t *w, uint16_t remaining, uint16_t count,
                        uint16_t *out)
{
    const uint32_t total = (uint32_t)w->scans * w->channels;
    /* Transfers made in this lap; a remaining count outside 1..total reads
       as the start of a lap. */
    const uint32_t written = (remaining >= 1u && remaining <= total) ? total - remaining : 0u;
    /* The scan being written, or about to be. */
    const uint32_t current = written / w->channels;

    if (count > w->scans - 1u) {
        count = (uint16_t)(w->scans - 1u);
    }

    for (uint8_t c = 0; c < w->channels; c++) {
        uint32_t sum = 0u;
        uint32_t n = 0u;

        for (uint32_t back = 1u; back <= count; back++) {
            const uint32_t scan = (current + w->scans - back) % w->scans;
            const uint16_t v = w->buf[scan * w->channels + c];

            if (v != ADC_WINDOW_EMPTY) {
                sum += v;
                n++;
            }
        }
        out[c] = (n == 0u) ? (uint16_t)ADC_WINDOW_EMPTY : (uint16_t)((sum + n / 2u) / n);
    }
}
