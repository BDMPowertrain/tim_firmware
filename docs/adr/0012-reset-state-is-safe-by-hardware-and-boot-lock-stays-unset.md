---
status: accepted
---

# The reset state is made safe by hardware pulls, and BOOT_LOCK stays unset

Every TIM GPIO floats while the MCU is in reset, in the ST system bootloader, with empty flash and after a watchdog reset. So every safety-relevant output has a hardware pull to its safe level on the daughterboard, and firmware is never what makes the reset state safe. With that rule a board that is unprogrammed or stuck in the bootloader is safe as it stands (BMS_OK low, no inverter command, the Control bus quiet), so the option bit that forbids the bootloader (BOOT_LOCK) is left at 0. Provisioning instead sets nSWBOOT0=0 and nBOOT0=1, so the pull-up on PB8 no longer selects the bootloader, and raises the brown-out threshold.

## Considered options

- **BOOT_LOCK=1.** Rejected: it buys nothing the pulls do not already give and is believed to be clearable only through a read-protection regression with mass erase (from memory, not verified against RM0440), a poor trade on a development board.
- **Firmware drives outputs safe early in startup.** Rejected as the mechanism: it does nothing for a board with no image, a dead crystal or an MCU held in reset. It is still done, as the first peripheral step.

## Consequences

- The BMS master already complies (R17 pulls MCU_BMS_OK low). The VCU daughterboard must pull the brake light, fault LED, Ready to Drive sound and any future enable output to "off".
- The bootloader drives PA6 (GPIO_1) and PA2 high, so neither may carry a signal whose high level is unsafe.
- The brake light is dark while the MCU is in reset. Accepted.
- The bootloader probably puts a 1.2-4 ms dominant burst on the DAQ bus. Accepted, because no safety function reads that bus; the pull-up on FDCAN2_TX stays a rev B finding.
- If the option bytes are ever corrupted the MCU restores the factory values and the board returns to the bootloader. That is safe, and provisioning must be repeated.
