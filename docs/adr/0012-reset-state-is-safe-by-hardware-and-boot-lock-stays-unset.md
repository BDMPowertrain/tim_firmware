---
status: accepted
---

# The reset state is made safe by hardware pulls, and BOOT_LOCK stays unset

Every TIM GPIO floats while the MCU is in reset, in the ST system bootloader, with empty flash and after a watchdog reset. So every safety-relevant output has a hardware pull to its safe level on the daughterboard, and firmware is never what makes the reset state safe. With that rule a board that is unprogrammed or stuck in the bootloader is safe as it stands (BMS_OK low, no inverter command, the Control bus quiet), so the option bit that forbids the bootloader (BOOT_LOCK) is left at 0. Provisioning instead sets nSWBOOT0=0 and nBOOT0=1, so the pull-up on PB8 no longer selects the bootloader, and raises the brown-out threshold.

## Considered options

- **BOOT_LOCK=1.** Rejected: it buys nothing the pulls do not already give. It can be cleared again at read-protection level 0, but at level 1 only by a regression to level 0 with a mass erase (RM0440 Rev 9 section 3.5.6), and a failed option load puts the part at level 1 (see Consequences). On a development board that risk buys nothing.
- **Firmware drives outputs safe early in startup.** Rejected as the mechanism: it does nothing for a board with no image, a dead crystal or an MCU held in reset. It is still done, as the first peripheral step.

## Consequences

- The BMS master already complies (R17 pulls MCU_BMS_OK low). The VCU daughterboard must pull the brake light, fault LED, Ready to Drive sound and any future enable output to "off".
- The bootloader drives PA6 (GPIO_1) and PA2 high, so neither may carry a signal whose high level is unsafe.
- The brake light is dark while the MCU is in reset. Accepted.
- The bootloader probably puts a 1.2-4 ms dominant burst on the DAQ bus. Accepted, because no safety function reads that bus; the pull-up on FDCAN2_TX stays a rev B finding.
- If the option bytes ever fail to load, the MCU does not restore the factory values (RM0440 Rev 9 section 3.4.2). It forces read protection to level 1, every user option bit to 1 (so nSWBOOT0=1 and the board boots the ST bootloader) and BOR_LEV to its lowest level. The board is safe as it stands. Returning it to service needs a regression to level 0, which mass-erases the flash and with it the parameters and event log (ADR 0014), and then provisioning again.
