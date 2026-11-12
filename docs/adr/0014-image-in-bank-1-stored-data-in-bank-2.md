---
status: accepted
---

# The image lives in bank 1 and all stored data in bank 2

The MCU keeps its factory dual-bank flash setting (2 x 256 KB, 2 KB pages). The image is confined to bank 1 and limited to 224 KB by the linker script; the first 32 KB of bank 1 is named as the future bootloader's, and until that effort the image is still linked at 0x0800_0000. Bank 2 holds parameter copy A (0x0804_0000, 4 KB), parameter copy B (0x0804_1000, 4 KB) and the event log (0x0804_2000 to 0x0805_1FFF, 64 KB); the remaining 184 KB (0x0805_2000 to 0x0807_FFFF) is reserved and left blank. At start-up each image reads the dual-bank option bit and, if the part is not dual-bank, makes no flash write, runs on defaults and says so.

The reason is the erase stall: a page erase takes up to 24.5 ms and stops the CPU when the code runs from the bank being erased (RM0440 Rev 9 §3.3.5, §3.3.8; DS12288 p.124). The BMS can tolerate about 35 ms without a kick (ADR 0013) and its control step has a 20 ms period (ADR 0010). With code in bank 1 and data in bank 2 an erase stalls nothing.

## Considered options

- **Single bank, 512 KB for code.** Rejected: every erase stalls the CPU for the whole erase of a 4 KB page, and ST gives no time for it.
- **Dual bank with data at the end of bank 1.** Rejected: same stall, up to 24.5 ms per page, with interrupts whose handlers sit in that bank waiting too.
- **Link the image at 0x0800_8000 now behind a stub that only jumps.** Rejected: it is hand-written boot code this spec would have to own and test, for no present benefit. The size budget is what the bootloader effort needs from today.
- **Stored data on the SD card.** Rejected: the card can be missing, full or slow, and the BMS may not use it at all.

## Consequences

- Each image is capped at 224 KB. The linker fails the build above that.
- Only the HAL flash functions are used, because they carry the workaround for erratum ES0430 §2.2.2 (data cache during dual-bank read-while-write).
- A reset or power loss during a write leaves that flash word undefined, and reading it can raise an NMI. The NMI handler looks at the failing address: in the bank 2 data regions it sets a flag and returns, the reader skips that event or parameter copy, and a half-written event-log page is erased again (ES0430 §2.2.4) with an event and a shell counter recording the repair. A bad parameter copy is left until the next save. A failing address in bank 1 goes to the crash routine.
- A normal flash of the image does not touch bank 2, so parameters and the event log survive it. A mass erase destroys both.
- "No stall" is an argument from the reference manual and must be shown on the bench: step timing measured while pages are erased back to back.
