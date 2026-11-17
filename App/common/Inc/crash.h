#ifndef APP_COMMON_INC_CRASH_H
#define APP_COMMON_INC_CRASH_H

/* crash — the one crash routine (spec #36, Watchdog and crash path; ADR
   0011, 0013). Every failure the image detects itself ends here: the
   stack-overflow hook, the fault handlers and the NMI, a failed assert
   (configASSERT, and the HAL's assert_failed() if it is ever enabled),
   the generated Error_Handler() and the shell's `crash`. It
   - turns interrupts off,
   - writes the Crash record (cause, task, address) to the backup
     registers (reset_info.h),
   - resets the MCU by software, through crash_reset(), always.
   It never stops on its own, even with a debugger attached: the debug
   enable bit outlives the probe until a power cycle, and a halt with no one
   there would freeze the board with its watchdog. A GDB session started
   with tools/tim.gdb has a breakpoint on crash_reset(), so it stops there
   with the record written.
   The next start reports it in `lastreset` and counts the reset as not
   power-on (on the BMS a Held fault, ADR 0011).

   The task is the one running when the failure was found: "ISR" if the
   failing code was an interrupt handler, "main" before the scheduler ran.

   A failed allocation is not a crash: its hook sets crash_alloc_failed()
   and returns, and the caller handles the NULL it gets. */

#include <stdbool.h>
#include <stdint.h>

#include "reset_record.h"

/* The crash routine, from anywhere. address is where the code was: a
   caller's return address, for example. */
__attribute__((noreturn)) void crash_now(crash_cause_t cause, uint32_t address);

/* For the fault handlers and the NMI: exc_return and sp are LR and SP as
   CRASH_CAPTURE() read them. The address recorded is the PC the hardware
   stacked: the instruction that faulted, 0 if no frame was found. */
__attribute__((noreturn)) void crash_fault(crash_cause_t cause, uint32_t exc_return,
                                           uint32_t sp);

/* Reads LR and SP into two uint32_t. The first statement of a fault
   handler's USER CODE: LR still holds EXC_RETURN there, as no call has been
   made yet. */
#define CRASH_CAPTURE(lr, sp) \
    __asm volatile("mov %0, lr\n\tmov %1, sp" : "=r"(lr), "=r"(sp))

/* A whole fault handler's USER CODE: capture, then crash. */
#define CRASH_FROM_FAULT(cause)                    \
    do {                                           \
        uint32_t crashLr_;                         \
        uint32_t crashSp_;                         \
        CRASH_CAPTURE(crashLr_, crashSp_);         \
        crash_fault((cause), crashLr_, crashSp_);  \
    } while (0)

/* configASSERT's action (FreeRTOSConfig.h). The address recorded is the
   assert's own. */
__attribute__((noreturn, noinline)) void crash_assert(void);

/* The crash routine's last step: a software reset. A function of its own
   only so that tools/tim.gdb can put a breakpoint on it. */
__attribute__((noreturn, noinline)) void crash_reset(void);

/* A FreeRTOS allocation has failed since start-up (the failed-allocation
   hook). An error bit: nothing is reset for it. */
bool crash_alloc_failed(void);

#endif /* APP_COMMON_INC_CRASH_H */
