/* reset_handler.s — the image's Reset_Handler, common to both images
   (spec #36, Start-up and reset).

   The generated one (targets/<image>/startup_stm32g474xx.s) is a weak
   symbol; this strong one replaces it at link time, so no generated file
   is edited or left out. It is the generated one with the ES0430 2.2.6
   workaround in front:

   ES0430 2.2.6, SRAM write error: a system reset at a critical instant can
   upset an SRAM's state machine; the first access after the reset
   restores it, but if that first access is a write, the write is lost. So
   each 32 KB SRAM block gets one dummy READ before anything writes RAM:
   before SystemInit, whose C code may push to the stack. SRAM parity is
   off (SRAM_PE = 1, the factory value Provisioning leaves alone), so
   reading a never-written word is safe; with parity on, ES0430 asks for a
   dummy write-initialisation instead, and this would have to change.

   After every CubeMX regeneration, diff the generated Reset_Handler (git
   diff -- targets/<image>/startup_stm32g474xx.s) and carry any change into
   this one, in its own commit (CLAUDE.md, Regeneration ritual). */

  .syntax unified
  .cpu cortex-m4
  .fpu softvfp
  .thumb

  .global Reset_Handler

  .section .text.Reset_Handler,"ax",%progbits
  .type Reset_Handler, %function
Reset_Handler:
  ldr   r0, =_estack
  mov   sp, r0          /* set stack pointer: a register, no RAM write */

/* ES0430 2.2.6: one read of each SRAM block, before any RAM write. */
  ldr   r0, =0x20000000 /* SRAM1, first 32 KB */
  ldr   r1, [r0]
  ldr   r0, =0x20008000 /* SRAM1, second 32 KB */
  ldr   r1, [r0]
  ldr   r0, =0x20010000 /* SRAM1, last 16 KB */
  ldr   r1, [r0]
  ldr   r0, =0x20014000 /* SRAM2 */
  ldr   r1, [r0]
  ldr   r0, =0x10000000 /* CCM SRAM */
  ldr   r1, [r0]

/* From here as generated. */
/* Call the clock system initialization function.*/
  bl    SystemInit

/* Copy the data segment initializers from flash to SRAM */
  ldr   r0, =_sdata
  ldr   r1, =_edata
  ldr   r2, =_sidata
  movs  r3, #0
  b     LoopCopyDataInit

CopyDataInit:
  ldr   r4, [r2, r3]
  str   r4, [r0, r3]
  adds  r3, r3, #4

LoopCopyDataInit:
  adds  r4, r0, r3
  cmp   r4, r1
  bcc   CopyDataInit

/* Zero fill the bss segment. */
  ldr   r2, =_sbss
  ldr   r4, =_ebss
  movs  r3, #0
  b     LoopFillZerobss

FillZerobss:
  str   r3, [r2]
  adds  r2, r2, #4

LoopFillZerobss:
  cmp   r2, r4
  bcc   FillZerobss

/* Call static constructors */
  bl    __libc_init_array
/* Call the application's entry point.*/
  bl    main

LoopForever:
  b     LoopForever

  .size Reset_Handler, .-Reset_Handler
