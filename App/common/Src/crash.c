/* crash.c — see crash.h. Also the FreeRTOS stack-overflow and
   failed-allocation hooks, which are the same in both images. */

#include "crash.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "reset_info.h"

/* EXC_RETURN bits (ARMv7-M ARM B1.5.8): bit 2 set, the stacked frame is
   on PSP; bit 3 set, the exception came from Thread mode. */
#define EXC_RETURN_PSP    (1u << 2)
#define EXC_RETURN_THREAD (1u << 3)

/* Words above the handler's SP searched for EXC_RETURN in an MSP frame:
   the prologue pushes a few registers, LR last at the top. */
#define PROLOGUE_WORDS_MAX 16u

/* All of SRAM1, SRAM2 and the CCM SRAM alias (RM0440 2.2): a stacked
   frame outside it is not read, so a broken SP cannot fault us again. */
#define SRAM_START 0x20000000u
#define SRAM_END   0x20020000u

/* The hardware frame: R0-R3, R12, LR, PC, xPSR. */
#define FRAME_WORDS 8u
#define FRAME_PC    6u
#define FRAME_XPSR  7u

/* A stacked PC is in the image (bank 1 code) and a stacked xPSR has the
   Thumb bit set: how a found frame is told from a stale EXC_RETURN in the
   handler's uninitialised locals. */
#define FLASH_START  0x08000000u
#define FLASH_END    0x08080000u
#define XPSR_THUMB   (1u << 24)

static volatile bool allocFailed;
/* Set on entry: a fault inside the crash routine resets at once. */
static volatile bool crashing;

static bool in_sram(uint32_t start, uint32_t words)
{
    return start >= SRAM_START && start <= SRAM_END - words * 4u && (start & 3u) == 0u;
}

/* Interrupts off, and a second entry (a fault inside the crash routine)
   resets at once, keeping whatever record the first one wrote. */
static void enter(void)
{
    __disable_irq();
    if (crashing) {
        NVIC_SystemReset();
    }
    crashing = true;
}

/* The task to name. The TCB is in RAM and read only for its name. */
static void name_task(char out[CRASH_TASK_CHARS + 1u], bool in_handler)
{
    const char *name;

    if (in_handler) {
        name = "ISR";
    } else if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        name = "main";
    } else {
        name = pcTaskGetName(NULL);
    }
    strncpy(out, name != NULL ? name : "?", CRASH_TASK_CHARS);
    out[CRASH_TASK_CHARS] = '\0';
}

__attribute__((noreturn)) static void reset_or_break(void)
{
    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0u) {
        /* "With a debugger attached it breaks instead." Only with the bit
           set: without it a BKPT is itself a HardFault. */
        __BKPT(0);
    }
    NVIC_SystemReset();
}

void crash_now(crash_cause_t cause, uint32_t address)
{
    enter();

    crash_record_t record = { .cause = cause, .address = address & ~1u };

    name_task(record.task, __get_IPSR() != 0u);
    reset_info_crash(&record);
    reset_or_break();
}

void crash_assert(void)
{
    crash_now(CRASH_ASSERT, (uint32_t)(uintptr_t)__builtin_return_address(0));
}

static bool looks_like_frame(const uint32_t *frame)
{
    return frame[FRAME_PC] >= FLASH_START && frame[FRAME_PC] < FLASH_END
           && (frame[FRAME_XPSR] & XPSR_THUMB) != 0u;
}

/* The stacked frame: exact on PSP (every task). On MSP (an interrupt, or
   main() before the scheduler) the handler's prologue has pushed onto it
   since, LR last, so the frame starts just above the word holding
   EXC_RETURN; the handler's uninitialised locals below it may hold a stale
   EXC_RETURN too, so a candidate counts only if it looks like a frame.
   NULL if none is found in RAM, or if the hardware could not stack it. */
static const uint32_t *stacked_frame(uint32_t exc_return, uint32_t sp)
{
    if ((SCB->CFSR & (SCB_CFSR_STKERR_Msk | SCB_CFSR_MSTKERR_Msk)) != 0u) {
        return NULL; /* the stacking itself faulted: the frame is not there */
    }
    if ((exc_return & EXC_RETURN_PSP) != 0u) {
        const uint32_t psp = __get_PSP();

        return in_sram(psp, FRAME_WORDS) ? (const uint32_t *)psp : NULL;
    }
    for (uint32_t i = 0; i < PROLOGUE_WORDS_MAX; i++) {
        const uint32_t at = sp + 4u * i;
        const uint32_t frame = at + 4u;

        if (!in_sram(at, 1u) || !in_sram(frame, FRAME_WORDS)) {
            return NULL;
        }
        if (*(const uint32_t *)at == exc_return && looks_like_frame((const uint32_t *)frame)) {
            return (const uint32_t *)frame;
        }
    }
    return NULL;
}

void crash_fault(crash_cause_t cause, uint32_t exc_return, uint32_t sp)
{
    enter();

    crash_record_t record = { .cause = cause };

    /* The record first, without the address, so that if reading the
       stacked frame faults, the cause and task are already kept. */
    name_task(record.task, (exc_return & EXC_RETURN_THREAD) == 0u);
    reset_info_crash(&record);

    const uint32_t *frame = stacked_frame(exc_return, sp);

    if (frame != NULL) {
        record.address = frame[FRAME_PC] & ~1u;
        reset_info_crash(&record);
    }
    reset_or_break();
}

bool crash_alloc_failed(void)
{
    return allocFailed;
}

/* configCHECK_FOR_STACK_OVERFLOW (FreeRTOSConfig.h): the task named has
   run off its stack. Its TCB holds the name; the stack itself is not
   trusted, and this runs in the context switch, on MSP. No address means
   anything: the overflow is found at a task switch, after the fact. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    enter();

    crash_record_t record = { .cause = CRASH_STACK_OVERFLOW };

    strncpy(record.task, pcTaskName != NULL ? pcTaskName : "?", CRASH_TASK_CHARS);
    record.task[CRASH_TASK_CHARS] = '\0';
    reset_info_crash(&record);
    reset_or_break();
}

/* configUSE_MALLOC_FAILED_HOOK (FreeRTOSConfig.h): an error bit, and the
   caller gets its NULL and handles it. A queue or task that tasks_init()
   could not create already leaves the image in STARTUP. */
void vApplicationMallocFailedHook(void)
{
    allocFailed = true;
}
