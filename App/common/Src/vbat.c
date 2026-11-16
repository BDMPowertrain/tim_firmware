/* vbat.c — VBAT (battery voltage) sensing via ADC1_IN9 (PC3), through a
   1/13 resistor divider confirmed directly from the schematic (a
   design-note text block states "VBAT_ADC = VBAT * 1/13", cross-checked
   against the actual resistor values: R21=120k, R22=10k, sum=130k,
   10/130=1/13). Nominal VBAT is 12V. The divider's math supports
   reading up to ~42.9V before the ADC itself saturates, but that is
   NOT this board's real survivable input — the SMBJ15A TVS diode on
   the VBAT connector clamps at 24.4V max, well below the ADC-math
   figure. Treat 42.9V as an ADC-saturation reference number only.

   Sampling is Housekeeping's (issue 47): the VCU hands vbat_sample() to
   it as its 100 ms poll. One polled ADC conversion is microsecond-scale
   even at the 640.5-cycle sample time VBAT's divider impedance needs
   (CubeMX regen, see commit history), so it barely delays the heartbeat.

   ADC1 has no other consumer right now, so no shared-access mutex is
   needed here (unlike spi3_bus.c) — if a future mezzanine ADC1 channel
   (PA0-3) gets its own driver, this will need one. */

#include "vbat.h"

#include "main.h"

extern ADC_HandleTypeDef hadc1;

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

void vbat_sample(void)
{
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

bool vbat_init(void)
{
    return HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) == HAL_OK;
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
