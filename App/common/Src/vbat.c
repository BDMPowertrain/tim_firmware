/* vbat.c — VBAT (battery voltage) sensing via ADC1_IN9 (PC3), through a
   1/13 resistor divider confirmed directly from the schematic (a
   design-note text block states "VBAT_ADC = VBAT * 1/13", cross-checked
   against the actual resistor values: R21=120k, R22=10k, sum=130k,
   10/130=1/13). Nominal VBAT is 12V. The divider's math supports
   reading up to ~42.9V before the ADC itself saturates, but that is
   NOT this board's real survivable input — the SMBJ15A TVS diode on
   the VBAT connector clamps at 24.4V max, well below the ADC-math
   figure. Treat 42.9V as an ADC-saturation reference number only.

   Sampling is a periodic software timer (osTimer), not a dedicated
   task — Documentation/tim-rtos-architecture.md section 6, ladder step
   6, "software timers vs. dedicated tasks". This is only appropriate
   because the timer callback runs in FreeRTOS's own Timer Service task
   (shared by every software timer in the system, not just this one),
   and the work it does — one polled ADC conversion — is microsecond-
   scale even at the 640.5-cycle sample time VBAT's divider impedance
   needs (CubeMX regen, see commit history). A slow or blocking
   operation in a timer callback would delay every other software timer
   in the system; this one is fast enough that it doesn't.

   ADC1 has no other consumer right now, so no shared-access mutex is
   needed here (unlike spi3_bus.c) — if a future mezzanine ADC1 channel
   (PA0-3) gets its own driver, this will need one. */

#include "vbat.h"

#include "FreeRTOS.h"

#include "cmsis_os2.h"

#include "main.h"

extern ADC_HandleTypeDef hadc1;

#define VBAT_SAMPLE_PERIOD_MS 500u

/* raw * 3300mV/4095 (ADC full-scale) * 13 (divider) collapsed into one
   multiply-divide; 42900 = 3300 * 13. Integer-only, no floats — same
   --specs=nano.specs constraint as shell.c's mg/mdps conversions. */
#define VBAT_MV_NUMERATOR 42900u
#define VBAT_ADC_FULL_SCALE 4095u

/* Two independent word-aligned scalars, not a struct — unlike
   imu.c/can_if.c's cached-latest patterns, this needs no mutex: each is
   a 32-bit aligned write/read, atomic on this Cortex-M4 on its own.
   Reading both together in vbat_get_reading() isn't atomic as a pair
   (see the header comment for why that's fine here). */
static volatile uint32_t latestRaw;
static volatile uint32_t latestMillivolts;
static volatile bool haveReading;

static void vbat_timer_cb(void *argument)
{
    (void)argument;

    if (HAL_ADC_Start(&hadc1) != HAL_OK) {
        return;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) {
        HAL_ADC_Stop(&hadc1);
        return;
    }

    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    latestRaw = raw;
    latestMillivolts = (raw * VBAT_MV_NUMERATOR) / VBAT_ADC_FULL_SCALE;
    haveReading = true;
}

void vbat_init(void)
{
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) {
        Error_Handler();
    }

    const osTimerAttr_t timer_attr = {
        .name = "vbatTimer",
    };
    osTimerId_t timer = osTimerNew(vbat_timer_cb, osTimerPeriodic, NULL, &timer_attr);
    if (timer == NULL) {
        Error_Handler();
    }

    osTimerStart(timer, pdMS_TO_TICKS(VBAT_SAMPLE_PERIOD_MS));
}

bool vbat_get_reading(uint32_t *out_raw, uint32_t *out_mv)
{
    if (!haveReading) {
        return false;
    }
    *out_raw = latestRaw;
    *out_mv = latestMillivolts;
    return true;
}
