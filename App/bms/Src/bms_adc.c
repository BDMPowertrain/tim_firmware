/* bms_adc.c — the BMS's analog inputs, see bms_adc.h. The same shape as
   the VCU's vcu_adc.c, with one ADC. */

#include "bms_adc.h"

#include "cmsis_os2.h"

#include "adc_window.h"
#include "main.h"
#include "vbat.h"

extern ADC_HandleTypeDef hadc1;
extern TIM_HandleTypeDef htim3;

/* The scan, in the rank order of targets/bms/tim.ioc. */
enum { CH_BOARD_TEMP, CH_CURRENT1, CH_CURRENT2, CH_VBAT, CH_VREFINT, CHANNELS };

/* 32 ms of scans: a 10-scan read is done long before the DMA comes round
   to what it read. */
#define SCANS 32u

static volatile uint16_t buf[SCANS * CHANNELS];
static const adc_window_t window = { buf, SCANS, CHANNELS };

/* Ticks to wait for the first scan: the first trigger is 1 ms after the
   timer starts and a scan takes about 80 us. */
#define FIRST_SCAN_WAIT_TICKS 5u

static uint32_t transfers_made(void)
{
    return SCANS * CHANNELS - __HAL_DMA_GET_COUNTER(hadc1.DMA_Handle);
}

bool bms_adc_start(void)
{
    for (uint32_t i = 0; i < SCANS * CHANNELS; i++) {
        buf[i] = ADC_WINDOW_EMPTY;
    }
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) {
        return false;
    }
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)buf, SCANS * CHANNELS) != HAL_OK) {
        return false;
    }
    /* Nothing runs per sample. CubeMX enables every DMA vector on this
       project, so the channel's interrupts are switched off here. */
    __HAL_DMA_DISABLE_IT(hadc1.DMA_Handle, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
    __HAL_ADC_DISABLE_IT(&hadc1, ADC_IT_OVR);
    if (HAL_TIM_Base_Start(&htim3) != HAL_OK) {
        return false;
    }

    for (uint32_t t = 0u; t < FIRST_SCAN_WAIT_TICKS && transfers_made() < CHANNELS; t++) {
        (void)osDelay(1u);
    }
    const uint32_t written = transfers_made();
    if (written < CHANNELS || written >= (SCANS - 1u) * CHANNELS) {
        return false;
    }
    /* The erratum's first conversion (ES0430), set back to empty. */
    for (uint32_t c = 0; c < CHANNELS; c++) {
        buf[c] = ADC_WINDOW_EMPTY;
    }
    return true;
}

void bms_adc_read(bms_adc_counts_t *out)
{
    uint16_t a[CHANNELS];

    adc_window_average(&window, (uint16_t)__HAL_DMA_GET_COUNTER(hadc1.DMA_Handle),
                       BMS_ADC_WINDOW_SCANS, a);
    out->board_temp = a[CH_BOARD_TEMP];
    out->current[0] = a[CH_CURRENT1];
    out->current[1] = a[CH_CURRENT2];
    out->vbat = a[CH_VBAT];
    out->vrefint = a[CH_VREFINT];
}

void bms_adc_poll_vbat(void)
{
    bms_adc_counts_t counts;

    bms_adc_read(&counts);
    if (counts.vbat != ADC_WINDOW_EMPTY) {
        vbat_record(counts.vbat);
    }
}
