/* reset_info.c — see reset_info.h. Registers, not the HAL: it runs before
   HAL_Init(), and the HAL RTC module is not enabled. */

#include "reset_info.h"

#include "main.h"

#include "control_step.h"
#include "print.h"

/* Written once by reset_info_capture() before the scheduler starts, only
   read after that. */
static reset_report_t report;
static uint32_t csrFound;
static bool debuggerFound;

_Static_assert(RESET_RECORD_WORDS <= 32u, "TAMP has 32 backup registers");

/* TAMP_BKP0R to TAMP_BKP31R are consecutive words (RM0440 38.6.13). */
static volatile uint32_t *backup_registers(void)
{
    return &TAMP->BKP0R;
}

/* The TAMP registers need their APB clock (RTCAPBEN) and, to be written,
   the backup-domain write protection off (PWR_CR1.DBP, RM0440 6.1.3). Both
   stay so until the next reset. */
static void open_backup_domain(void)
{
    SET_BIT(RCC->APB1ENR1, RCC_APB1ENR1_PWREN | RCC_APB1ENR1_RTCAPBEN);
    (void)READ_REG(RCC->APB1ENR1); /* the clock is on before the next access */
    SET_BIT(PWR->CR1, PWR_CR1_DBP);
    while (READ_BIT(PWR->CR1, PWR_CR1_DBP) == 0u) {
    }
}

void reset_info_capture(void)
{
    /* First: nothing has cleared or added to the flags yet. */
    csrFound = READ_REG(RCC->CSR);
    debuggerFound = (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0u;

    const reset_cause_t cause = reset_cause_classify(csrFound);

    open_backup_domain();

    if (reset_cause_is_power_on(cause)) {
        /* ES0430 2.2.5 workaround: reset the backup domain at every
           Power-on reset, so nothing a missed hardware reset left survives
           into the record. Write BDRST, read it back, release it. */
        WRITE_REG(RCC->BDCR, RCC_BDCR_BDRST);
        (void)READ_REG(RCC->BDCR);
        WRITE_REG(RCC->BDCR, 0u);
    }

    uint32_t words[RESET_RECORD_WORDS];
    volatile uint32_t *bkp = backup_registers();

    for (uint32_t i = 0; i < RESET_RECORD_WORDS; i++) {
        words[i] = bkp[i];
    }
    reset_record_boot(words, cause, &report);
    for (uint32_t i = 0; i < RESET_RECORD_WORDS; i++) {
        bkp[i] = words[i];
    }

    /* Kept above; now cleared, so the next start sees only its own cause. */
    SET_BIT(RCC->CSR, RCC_CSR_RMVF);
}

const reset_report_t *reset_info_report(void)
{
    return &report;
}

bool reset_info_debugger(void)
{
    return debuggerFound;
}

void reset_info_breadcrumb(const breadcrumb_t *breadcrumb)
{
    uint32_t words[RESET_RECORD_WORDS];
    volatile uint32_t *bkp = backup_registers();

    reset_record_put_breadcrumb(words, breadcrumb);
    for (uint32_t i = RESET_RECORD_BREADCRUMB_FIRST;
         i < RESET_RECORD_BREADCRUMB_FIRST + RESET_RECORD_BREADCRUMB_COUNT; i++) {
        bkp[i] = words[i];
    }
}

void reset_info_crash(const crash_record_t *crash)
{
    uint32_t words[RESET_RECORD_WORDS];
    volatile uint32_t *bkp = backup_registers();

    /* Again, in case anything since start-up turned the protection back
       on: the crash routine gets one try. */
    SET_BIT(PWR->CR1, PWR_CR1_DBP);
    reset_record_put_crash(words, crash);
    for (uint32_t i = RESET_RECORD_CRASH_FIRST;
         i < RESET_RECORD_CRASH_FIRST + RESET_RECORD_CRASH_COUNT; i++) {
        bkp[i] = words[i];
    }
}

static void print_breadcrumb(const breadcrumb_t *b)
{
    switch (b->phase) {
    case BREADCRUMB_PHASE_NONE:
        shell_printf("breadcrumb:   no control step ran\r\n");
        return;
    case BREADCRUMB_PHASE_START:
        shell_printf("breadcrumb:   step %lu, at its start (Kicked, no phase yet)",
                     (unsigned long)b->step_count);
        break;
    case BREADCRUMB_PHASE_END:
        shell_printf("breadcrumb:   step %lu, every phase done", (unsigned long)b->step_count);
        break;
    default: {
        const char *name = control_step_phase_name(b->phase);

        shell_printf("breadcrumb:   step %lu, in phase %u (%s)", (unsigned long)b->step_count,
                     (unsigned)b->phase, name != NULL ? name : "?");
        break;
    }
    }
    shell_printf(", %lu overruns\r\n", (unsigned long)b->overruns);
}

void reset_info_print(void)
{
    const reset_report_t *r = &report;

    shell_printf("last reset:   %s (RCC_CSR 0x%08lX)%s\r\n", reset_cause_name(r->cause),
                 (unsigned long)csrFound,
                 debuggerFound ? ", debugger attached since power-on" : "");
    shell_printf("not power-on: %lu since the last power-on reset\r\n",
                 (unsigned long)r->non_power_on_resets);
    if (!r->layout_found) {
        shell_printf("record:       backup registers held no record of ours\r\n");
        return;
    }
    if (reset_cause_is_power_on(r->cause)) {
        return;
    }
    print_breadcrumb(&r->breadcrumb);
    if (r->crash.cause == CRASH_NONE) {
        shell_printf("crash record: none\r\n");
    } else {
        shell_printf("crash record: %s in %s, ", crash_cause_name(r->crash.cause),
                     r->crash.task[0] != '\0' ? r->crash.task : "?");
        if (r->crash.address == 0u) {
            shell_printf("address unknown\r\n");
        } else {
            shell_printf("at 0x%08lX\r\n", (unsigned long)r->crash.address);
        }
    }
}
