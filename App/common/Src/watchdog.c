/* watchdog.c — see watchdog.h. Registers, not the HAL IWDG driver: the
   watchdog is not configured in CubeMX, so it starts in our code straight
   after GPIO init (RM0440 section 41, IWDG). */

#include "watchdog.h"

#include "main.h"

/* Key register values (RM0440 41.4.1). Our own names, so that enabling
   the HAL IWDG module in CubeMX one day cannot clash with them. */
#define WDG_KEY_START  0xCCCCu
#define WDG_KEY_ACCESS 0x5555u
#define WDG_KEY_RELOAD 0xAAAAu

/* LSI 32 kHz nominal / 4 = 8 kHz, so 480 counts is 60 ms. Timeout is
   4 x (RL + 1) / f_LSI: 56.5 ms at 34 kHz, 65.1 ms at 29.5 kHz. */
#define WDG_PRESCALER_DIV4 0u
#define WDG_RELOAD_60MS    479u
/* WINR at its maximum is the window feature off (RM0440 41.3.4). */
#define WDG_WINDOW_OFF     0x0FFFu

/* Polls of IWDG_SR before giving up. A register update crosses into the
   LSI domain and takes a few LSI cycles, well under a millisecond, so on
   a working part the loop ends long before this. The bound only matters
   if the update never completes: then the watchdog still runs on its
   reset values (about 512 ms) and watchdog_started() says so. */
#define WDG_SR_POLLS 200000u

static bool configured;

void watchdog_start(void)
{
    /* Frozen while the core is halted, in every build (spec, Watchdog and
       crash path). DBGMCU needs no clock enable on the G4. */
    SET_BIT(DBGMCU->APB1FZR1, DBGMCU_APB1FZR1_DBG_IWDG_STOP);

    IWDG->KR = WDG_KEY_START;
    IWDG->KR = WDG_KEY_ACCESS;
    IWDG->PR = WDG_PRESCALER_DIV4;
    IWDG->RLR = WDG_RELOAD_60MS;

    uint32_t polls = 0u;
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0u && polls < WDG_SR_POLLS) {
        polls++;
    }

    /* WINR goes last, once the prescaler and reload are in. */
    IWDG->WINR = WDG_WINDOW_OFF;
    while ((IWDG->SR & IWDG_SR_WVU) != 0u && polls < WDG_SR_POLLS) {
        polls++;
    }

    configured = (IWDG->SR == 0u);

    /* Part of the start, not a Kick: the counter starts from the new
       60 ms reload value. The HAL does the same when WINR is left at its
       reset value, which may not reload the counter on its own. */
    IWDG->KR = WDG_KEY_RELOAD;
}

bool watchdog_started(void)
{
    return configured;
}

void watchdog_kick(void)
{
    IWDG->KR = WDG_KEY_RELOAD;
}
