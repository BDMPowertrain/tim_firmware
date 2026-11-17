/* vcu_adc.c — the VCU's analog inputs, see vcu_adc.h. */

#include "vcu_adc.h"

#include "cmsis_os2.h"

#include "adc_window.h"
#include "main.h"
#include "vbat.h"

extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern TIM_HandleTypeDef htim3;

/* Each ADC's scan, in the rank order of targets/vcu/tim.ioc. */
enum { ADC1_APPS1, ADC1_BSE_FRONT, ADC1_SUPPLY1, ADC1_VBAT, ADC1_CHANNELS };
enum { ADC2_APPS2, ADC2_BSE_REAR, ADC2_SUPPLY2, ADC2_CHANNELS };

/* 32 ms of scans: a 10-scan read is done long before the DMA comes round
   to what it read. */
#define SCANS 32u

static volatile uint16_t adc1Buf[SCANS * ADC1_CHANNELS];
static volatile uint16_t adc2Buf[SCANS * ADC2_CHANNELS];

static const adc_window_t adc1Window = { adc1Buf, SCANS, ADC1_CHANNELS };
static const adc_window_t adc2Window = { adc2Buf, SCANS, ADC2_CHANNELS };

/* Starts one ADC's circular DMA with every interrupt it would enable
   switched off again: nothing runs per sample. Their NVIC lines are not
   enabled either (tim.ioc), so this is belt and braces. */
static bool start_dma(ADC_HandleTypeDef *hadc, volatile uint16_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = ADC_WINDOW_EMPTY;
    }
    if (HAL_ADCEx_Calibration_Start(hadc, ADC_SINGLE_ENDED) != HAL_OK) {
        return false;
    }
    if (HAL_ADC_Start_DMA(hadc, (uint32_t *)buf, len) != HAL_OK) {
        return false;
    }
    __HAL_DMA_DISABLE_IT(hadc->DMA_Handle, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
    __HAL_ADC_DISABLE_IT(hadc, ADC_IT_OVR);
    return true;
}

/* Transfers the DMA has made into buf since it started (before it wraps). */
static uint32_t transfers_made(ADC_HandleTypeDef *hadc, uint32_t channels)
{
    return SCANS * channels - __HAL_DMA_GET_COUNTER(hadc->DMA_Handle);
}

/* Sets the first scan back to the empty marker once the DMA has moved
   past it. False if it has not (the ADC or the timer is not running), or
   has already come round (impossible a few ms in). */
static bool discard_first_scan(ADC_HandleTypeDef *hadc, volatile uint16_t *buf,
                               uint32_t channels)
{
    const uint32_t written = transfers_made(hadc, channels);

    if (written < channels || written >= (SCANS - 1u) * channels) {
        return false;
    }
    for (uint32_t c = 0; c < channels; c++) {
        buf[c] = ADC_WINDOW_EMPTY;
    }
    return true;
}

/* Ticks to wait for the first scan: the first trigger is 1 ms after the
   timer starts and its scan takes about 35 us, but a 1 ms delay can
   wake up to a tick early, so it polls. */
#define FIRST_SCAN_WAIT_TICKS 5u

bool vcu_adc_start(void)
{
    bool ok = start_dma(&hadc1, adc1Buf, SCANS * ADC1_CHANNELS);
    ok = start_dma(&hadc2, adc2Buf, SCANS * ADC2_CHANNELS) && ok;
    ok = ok && HAL_TIM_Base_Start(&htim3) == HAL_OK;
    if (!ok) {
        return false;
    }

    for (uint32_t t = 0u; t < FIRST_SCAN_WAIT_TICKS; t++) {
        if (transfers_made(&hadc1, ADC1_CHANNELS) >= ADC1_CHANNELS
            && transfers_made(&hadc2, ADC2_CHANNELS) >= ADC2_CHANNELS) {
            break;
        }
        (void)osDelay(1u);
    }

    ok = discard_first_scan(&hadc1, adc1Buf, ADC1_CHANNELS);
    ok = discard_first_scan(&hadc2, adc2Buf, ADC2_CHANNELS) && ok;
    return ok;
}

void vcu_adc_read_sensors(vcu_sensor_raw_t *raw)
{
    uint16_t a1[ADC1_CHANNELS];
    uint16_t a2[ADC2_CHANNELS];

    adc_window_average(&adc1Window, (uint16_t)__HAL_DMA_GET_COUNTER(hadc1.DMA_Handle),
                       VCU_ADC_WINDOW_SCANS, a1);
    adc_window_average(&adc2Window, (uint16_t)__HAL_DMA_GET_COUNTER(hadc2.DMA_Handle),
                       VCU_ADC_WINDOW_SCANS, a2);

    raw->apps[0] = a1[ADC1_APPS1];
    raw->apps[1] = a2[ADC2_APPS2];
    raw->brake[0] = a1[ADC1_BSE_FRONT];
    raw->brake[1] = a2[ADC2_BSE_REAR];
    raw->supply[0] = a1[ADC1_SUPPLY1];
    raw->supply[1] = a2[ADC2_SUPPLY2];
}

void vcu_adc_poll_vbat(void)
{
    uint16_t a1[ADC1_CHANNELS];

    adc_window_average(&adc1Window, (uint16_t)__HAL_DMA_GET_COUNTER(hadc1.DMA_Handle),
                       VCU_ADC_WINDOW_SCANS, a1);
    if (a1[ADC1_VBAT] != ADC_WINDOW_EMPTY) {
        vbat_record(a1[ADC1_VBAT]);
    }
}
