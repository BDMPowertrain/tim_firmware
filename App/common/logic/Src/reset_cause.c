/* reset_cause.c — see reset_cause.h. */

#include "reset_cause.h"

#include <stddef.h>

/* RCC_CSR flags, RM0440 7.4.28. Our own names: logic sees no CMSIS header. */
#define CSR_OBLRSTF  (1u << 25)
#define CSR_PINRSTF  (1u << 26)
#define CSR_BORRSTF  (1u << 27)
#define CSR_SFTRSTF  (1u << 28)
#define CSR_IWDGRSTF (1u << 29)
#define CSR_WWDGRSTF (1u << 30)
#define CSR_LPWRRSTF (1u << 31)

/* Highest priority first. BOR first (ADR 0011); then the internal causes,
   the watchdog before software so a hang is never hidden; the pin last. */
static const struct {
    uint32_t      flag;
    reset_cause_t cause;
} byPriority[] = {
    { CSR_BORRSTF,  RESET_CAUSE_POWER_ON },
    { CSR_IWDGRSTF, RESET_CAUSE_WATCHDOG },
    { CSR_WWDGRSTF, RESET_CAUSE_WINDOW_WATCHDOG },
    { CSR_SFTRSTF,  RESET_CAUSE_SOFTWARE },
    { CSR_OBLRSTF,  RESET_CAUSE_OPTION_LOAD },
    { CSR_LPWRRSTF, RESET_CAUSE_LOW_POWER },
    { CSR_PINRSTF,  RESET_CAUSE_PIN },
};

reset_cause_t reset_cause_classify(uint32_t rcc_csr)
{
    for (size_t i = 0; i < sizeof(byPriority) / sizeof(byPriority[0]); i++) {
        if ((rcc_csr & byPriority[i].flag) != 0u) {
            return byPriority[i].cause;
        }
    }
    return RESET_CAUSE_UNKNOWN;
}

bool reset_cause_is_power_on(reset_cause_t cause)
{
    return cause == RESET_CAUSE_POWER_ON;
}

const char *reset_cause_name(reset_cause_t cause)
{
    switch (cause) {
    case RESET_CAUSE_UNKNOWN:         return "unknown";
    case RESET_CAUSE_POWER_ON:        return "power-on";
    case RESET_CAUSE_WATCHDOG:        return "watchdog";
    case RESET_CAUSE_WINDOW_WATCHDOG: return "window watchdog";
    case RESET_CAUSE_SOFTWARE:        return "software";
    case RESET_CAUSE_OPTION_LOAD:     return "option load";
    case RESET_CAUSE_LOW_POWER:       return "low-power";
    case RESET_CAUSE_PIN:             return "pin";
    default:                          return "?";
    }
}
