# STM32G474 reset, flash, FDCAN and watchdog facts

Answers issue #6. Part TIM rev A: STM32G474RET3, LQFP64, 512 KB flash.

## Read this first: what was and was not read

| Source | Revision | Read? |
|---|---|---|
| DS12288, STM32G474xB/xC/xE datasheet (`Datasheets/stm32g474cb.pdf`) | Rev 6, November 2021 (footer, p.1) | Yes |
| ST CMSIS device header `Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h` | CMSIS STM32G4 V1.2.6 (`stm32g4xx.h` L93-96) | Yes |
| ST HAL/LL headers and sources in `Drivers/STM32G4xx_HAL_Driver/` | HAL V1.2.7 (`stm32g4xx_hal.c` L53-55) | Yes |
| RM0440, STM32G4 reference manual (`Datasheets/rm0440-...pdf`, local, untracked) | **Rev 9, March 2025** (footer, p.1) | **Yes** (second pass) |
| ES0430, STM32G471/473/474/483/484 errata sheet (`Datasheets/es0430-...pdf`, local, untracked) | **Rev 10, November 2026** (02-Nov-2026, revision history p.35) | **Yes** (third pass) |
| AN2606, STM32 system memory boot mode (`Datasheets/an2606-...pdf`, local, untracked) | **Rev 71** (02-Nov-2026, revision history) | **Yes** (third pass): §4 general, §53 STM32G47xxx/48xxx, Tables 2, 3, 239, 240 |
| ADM3055E/ADM3057E data sheet (`Datasheets/adm3055e-adm3057e.pdf`), for the bootloader pin analysis only | Rev. D | Pin table and dominant-timeout spec only |

st.com did not respond from this machine: curl, PowerShell and WebFetch all
timed out with 0 bytes received, while github.com answered at once. The site
appears to block automated clients, and that block was not bypassed. No local
copies were found under Downloads, Documents or `~/Git`.

**Second pass (2026-11-11).** RM0440 Rev 9 was supplied as a local PDF and
read. ES0430 and AN2606 were tried again from st.com (WebFetch three times,
curl twice): every attempt timed out or the connection was reset. Mirrors were
deliberately not used. Both documents are still missing.

**Third pass (2026-11-11).** ES0430 Rev 10 and AN2606 Rev 71 were supplied as
local PDFs and read. §7 (errata) and the bootloader part of §1 now come from
them. The pin collisions in §1 were cross-checked against `tim.ioc` in this
branch.

So the fallback was ST's own CMSIS/HAL source, which ships with the project.
It is first-party ST material and authoritative for **register bit positions
and option-byte encodings**. Its comments are secondary for **behaviour**, so
behaviour facts taken only from a HAL comment are marked as such. Where
RM0440 now covers the same fact, the RM citation is added.

Items still marked **UNVERIFIED** need a bench measurement or a document
that is not in `Datasheets/`. They are listed at the end.

Citation forms used below:
- `DS p.N` is DS12288 Rev 6, printed page N of 236.
- `RM §x p.N` is RM0440 Rev 9, section x, printed page N of 2140 (the page
  footer, which matches the PDF page number).
- `ES §x p.N` is ES0430 Rev 10, section x, page N of 39.
- `AN §x p.N` is AN2606 Rev 71, section x, page N of 584.
- `ADM p.N` is the ADM3055E/ADM3057E data sheet Rev. D, page N of 27.
- `CMSIS Lnnnn` is a line in `stm32g474xx.h`.
- `HAL <file> Lnnn` is a line in the named HAL file.

**Category 3 confirmed.** RM0440 Table 1 lists STM32G474 with 512 KB as a
category 3 device (RM §1.5 p.75). Every flash fact below is from the
category 3 FLASH chapter (RM §3, p.94-148).

### Corrections to the first pass (from RM0440)
- **CCM SRAM is NOT erased on system reset on a factory-fresh part.** The ST
  production value of the user option bytes has `CCMSRAM_RST = 1` ("not
  erased") (RM §3.4.1 p.111-112). The first pass said it is erased by default.
- **The bus-off recovery wait is stated two ways in RM0440:** 128 x 11
  recessive bits (RM §44.3.5 p.1966) and 129 x 11 after software clears INIT
  (RM §44.4.13 p.1994). See §4.
- **DS conflict on reset pin state resolved:** RM says most I/Os are in
  **analog** mode during and just after reset (RM §9.3.1 p.348), and the
  `GPIOx_MODER` reset values confirm it (RM §9.4.1 p.355).

---

## 1. Pin states through reset and the bootloader

### General I/O
- **During and after reset**, DS12288 gives two descriptions that do not agree:
  - §3.11.5 "Reset mode": the I/Os are in **"analog state"** (Schmitt trigger
    disabled) both under and after reset (DS p.25).
  - Table 11: "Unless otherwise specified by a note, all I/Os are set as
    **floating inputs** during and after reset" (DS p.56).

  Either way, no pin drives and none has a pull, apart from the exceptions
  below.
- **RM0440 settles it: analog.** "During and just after reset, the alternate
  functions are not active and most of the I/O ports are configured in analog
  mode" (RM §9.3.1 p.348). Reset values (RM §9.4.1-9.4.4 p.355-356):

  | Register | Port A | Port B | Ports C..G |
  |---|---|---|---|
  | `GPIOx_MODER` | 0xABFF FFFF | 0xFFFF FEBF | 0xFFFF FFFF |
  | `GPIOx_OSPEEDR` | 0x0C00 0000 | 0x0000 0000 | 0x0000 0000 |
  | `GPIOx_PUPDR` | 0x6400 0000 | 0x0000 0100 | 0x0000 0000 |

  Decoded: every pin is analog (MODER = 11) except PA13, PA14, PA15, PB3 and
  PB4, which are AF (10). Pulls: PA15 up, PA14 down, PA13 up, PB4 up. PA13 is
  very-high speed. `GPIOx_AFRL/AFRH` reset to AF0 (RM §9.3.2 p.348).
- **After any system reset** (NRST, IWDG, WWDG, software, OBL, BOR) all
  registers return to their reset values unless the register description says
  otherwise (RM §7.1.2 p.270). The GPIO registers are not excepted, so an IWDG
  reset puts every pin back to analog: **outputs stop driving** and LEDs,
  enables and chip selects float until firmware re-inits them. Design the
  external pulls for that.
- **NRST**: the internal reset pull-up is deactivated when the reset source is
  internal (DS p.25). Its weak pull-up R_PU is 25/40/55 kΩ min/typ/max, and the
  input filter passes pulses of 70 ns max (Table 58, DS p.133). The NRST pin
  mode is the option field `NRST_MODE[1:0]`: input-only, GPIO, or
  input+output (CMSIS L5288; HAL `stm32g4xx_hal_flash.h` L442-444).
- **NRST in the factory mode (reset input/output, `NRST_MODE = 11`)**: every
  internal reset source (IWDG, WWDG, software, low-power, OBL, POR) is driven
  out on NRST by a pulse generator with a **minimum 20 µs** pulse. This mode
  is always active during power-on reset until the option bytes load
  (RM §7.1.2 p.270-271). So anything else on the NRST net (a debugger, a
  daughterboard reset) sees every IWDG reset.
- **Internal pull-ups and pull-downs** are 25/40/55 kΩ min/typ/max
  (Table 54, DS p.128).

### SWD and JTAG pins (LQFP64: PA13 = pin 49, PA14 = pin 50, PA15, PB3, PB4)
- After reset these pins are already configured as the JTAG/SW debug
  alternate functions. PA15, PA13 and PB4 have their **internal pull-up on**,
  and PA14 has its **internal pull-down on** (Table 12 note 5, DS p.72).
  SWD therefore works out of reset without any firmware.
- RM0440 agrees and adds **PB3 (JTDO) is AF with no pull** (floating)
  (RM §9.3.1 p.348; reset values above).

### PB4 and PB6 (UCPD dead-battery pull-downs)
- After reset, a 5.1 kΩ pull-down (Rd) can appear on PB6 and PB4. It is
  enabled by a high level on PA9 (for PB6) or on PA10 (for PB4). Setting
  `PWR_CR3.UCPD1_DBDIS = 1` removes it (Table 12 note 6, DS p.72).
- **Check against the TIM schematic.** If PA9 or PA10 sits high at reset,
  PB4 or PB6 gets an unintended 5.1 kΩ pull-down until firmware sets
  `UCPD1_DBDIS`.

### PB8-BOOT0 (LQFP64 pin 61)
- I/O structure FT_f (5 V tolerant, Fm+ capable). Alternate functions include
  I2C1_SCL and FDCAN1_RX (Table 12, DS p.71).
- The BOOT0 level comes from either the PB8-BOOT0 pin or the `nBOOT0` option
  bit, depending on `nSWBOOT0` ("nBOOT_SEL" in the DS text) (§3.7, DS p.21).
- Table 12 note 7: set PB8 to a mode other than analog after startup to limit
  consumption if the pin is unconnected (DS p.72).
- **PB8 during reset (RM0440):** PB8 is in **input mode** during reset, at
  least until option-byte loading ends, then switches by itself to analog
  (RM §9.3.1 p.348). RM0440 states the timing two incompatible ways:
  - RM §2.6.1 p.92: with `nSWBOOT0 = 1` it is input for the whole reset
    phase and goes analog after reset release; with `nSWBOOT0 = 0` it is
    input until OBL completes and then goes analog, even before reset ends.
  - RM §9.3.15 p.354: analog "after the option byte loading phase if
    nSWBOOT0 = 1" and "after reset if nSWBOOT0 = 0" (the reverse).

  Either way PB8 never drives during reset. The exact window is not needed for
  this design, so the conflict is only noted.
- **BOOT0 and nBOOT1 are latched on the 4th edge of the internal startup clock
  after reset release, and re-sampled on exit from Standby** (RM §2.6.1
  p.92). A pin-selected BOOT0 must therefore also hold its level in Standby.
- **Inference, not from a document.** With a 2 kΩ pull-up to 3V3, an internal
  pull of 25 to 55 kΩ cannot pull PB8 low. With `nSWBOOT0 = 1`, BOOT0 reads 1,
  so the boot target depends on `nBOOT1` (see §2). That matches the CLAUDE.md
  symptom (PC in 0x1FFF_xxxx).

### PC13, PC14, PC15 (LQFP64 pins 2, 3, 4; VBAT is pin 1)
- These three pins are **supplied through the backup power switch**, which
  "only sinks a limited amount of current (3 mA)". In output mode:
  - speed must not exceed **2 MHz with a maximum load of 30 pF**;
  - they **must not be used as current sources** (for example to drive an
    LED). Sinking is not forbidden by this note.

  (Table 12 note 2, DS p.72)
- **The DS limit covers all three pins.** It is not only a "PC14 weak drive"
  limit. Sourcing current from PC13 or PC15 breaks the same note.
- After a backup-domain power-up, PC13 to PC15 operate as GPIOs. After that,
  their function depends on RTC/TAMP registers that a **system reset does not
  reset** (Table 12 note 3, DS p.72). An RTC_OUT, tamper or LSE setting can
  therefore survive a firmware reset and keep owning the pin.
- **VBAT unconnected.** The DS says VBAT may be supplied "from VDD when there
  is no external battery" (§3.11.6, DS p.26). The VBAT range is 1.55 to 3.6 V
  (§3.11.1, DS p.23). The DS gives no behaviour for a floating VBAT. How the
  power switch, the backup domain and PC13 to PC15 behave with VBAT floating
  is **UNVERIFIED**; RM0440's PWR chapter is the place to look. Treat the
  backup domain as not guaranteed on this board.
- **RM0440 on PC13-PC15 and VBAT (PWR, RM §6.1.3 p.230-231):**
  - Same limit as the DS, in RM words: the switch passes only 3 mA, so in
    output mode PC13-PC15 are limited to **2 MHz, 30 pF, and must not be
    used as a current source** (e.g. an LED). Nothing is added for PC14
    alone.
  - VBAT powers the RTC, the LSE oscillator and the PC13-PC15 I/Os. When VDD
    is present the switch connects the backup domain to VDD and PC13-PC15
    work as GPIOs; in VBAT mode they can only be controlled by RTC/LSE.
  - **"If no external battery is used in the application, it is recommended
    to connect VBAT externally to VDD with a 100 nF external ceramic
    decoupling capacitor."** RM0440 says nothing about a floating VBAT. A
    floating VBAT is therefore outside ST's documented use, not a
    characterised state.
  - **Warning in the RM:** during t_RSTTEMPO at VDD start-up, and after a PDR,
    the power switch stays connected to **VBAT**. If VDD rises faster than
    t_RSTTEMPO and VDD > VBAT + 0.6 V, current may be injected into VBAT
    through an internal diode. With VBAT floating, the backup domain and
    PC13-PC15 are fed from that floating node during start-up. That is an
    inference from the warning, not an ST statement, and is the strongest
    argument for tying VBAT to VDD on the next board revision.
- **PC13-PC15 in Standby:** the GPIO function is lost when the core domain is
  off; unless the RTC owns the pin, it goes to analog input (RM §9.3.14
  p.353).

### During the system bootloader (VERIFIED from AN2606 Rev 71)
The first two passes had only the DS and RM statement that the bootloader uses
"USART, SPI, I2C, USB" (DS p.21; RM §3.3.1 p.95). AN2606 §53 now gives the
detail.

**Identity.** Bootloader ID **0xD5**, stored at **0x1FFF 6FFE**; protocols
USART V3.1, I2C V1.2, SPI V1.1, DFU V2.2 (AN Table 3 p.43). The ID's high
nibble 0xD means "USART, DFU, I2C and SPI" (AN §4.2 p.38). Device ID 0x469
(AN Table 239 p.550). Versions (AN Table 119 p.288-289):
- V13.3 (0xD3): "Boot from bank2 is not working".
- V13.4 (0xD4): CCM SRAM not supported.
- V13.5 (0xD5): no known limitations.

Read 0x1FFF 6FFE over SWD on a TIM board to learn which one it carries.

**The G474 bootloader has no FDCAN interface and does not use I2C1.** Its
interfaces are USART1/2/3, I2C2/3/4, SPI1/2 and USB DFU only (AN Table 3 p.43;
AN Table 118 p.284-286). FDCAN1 (PA11/PA12 is shared with USB), FDCAN2
(PB5/PB6) and I2C1 (PB8) are never configured as CAN or I2C by it.

**Resources and pin configuration** (AN §53.1 Table 118 p.284-286):

| Item | Bootloader setting |
|---|---|
| Clock | HSI + PLL, SYSCLK **72 MHz**; CRS + HSI48 for USB |
| RAM | **16 KB from 0x2000 0000 used by the bootloader** |
| IWDG | Prescaler set to maximum and refreshed periodically, in case the hardware IWDG option is on |
| USART1/2/3 | 8 data bits, even parity, 1 stop bit. RX and TX pins in **AF push-pull with pull-up**: USART1 PA10/PA9, USART2 PA3/PA2, USART3 PC11/PC10 |
| I2C2/3/4 | Target mode, 7-bit address **0b1010011x** (0x53), up to 1 MHz, analog filter on. SCL/SDA in **open-drain with pull-up**: I2C2 PC4/PA8, I2C3 PC8/PC9, I2C4 PC6/PC7 |
| SPI1 | Slave, full duplex, 8-bit MSB, up to 8 MHz, CPOL 0/CPHA 0, hardware NSS. PA7 MOSI, PA6 MISO, PA5 SCK, PA4 NSS, all **with pull-down**; MISO is an output |
| SPI2 | Same as SPI1: PB15 MOSI, PB14 MISO, PB13 SCK, PB12 NSS, all **with pull-down**; MISO is an output |
| USB DFU | Forced device mode, no external pull-up needed; PA11 (DM) and PA12 (DP) in input, no pull |

Footnote 1 to Table 118: SPI MISO is DMA-driven, and **"as soon as the bit
DMATx enable ... is set ... the MISO line is set to 3.3 V"** (AN p.286). PA6
and PB14 are therefore **driven high** while the bootloader runs.

**Startup and detection sequence** (AN §53.2 Figure 69 p.287; Table 240
p.556):
1. After reset, set SYSCLK to 72 MHz from HSI, then initialise clock, GPIOs,
   IWDG and SysTick.
2. Configure USB device FS (HSI48 + CRS), then all I2Cx, SPIx and USARTx.
   Minimum startup time is **0.390 ms**.
3. Loop until one of these happens; the first interface to see its event
   wins, and the others are disabled:
   - **0x7F received** on any USARTx;
   - its I2C address seen on any I2Cx;
   - the SPI synchronisation sequence on any SPIx;
   - a USB cable detected.
- AN2606 warns: keep the RX pins of unused interfaces (USART_RX, SPI_MOSI,
  CAN_RX) at a fixed level, and D+/D- at the same level, during detection.
  "Leaving these pins floating during the detection phase can result in
  activation of unused interfaces." A USB host must not be connected during
  selection of another interface (AN §4.3 p.50).
- Readout protection Level 2 blocks boot into system memory (AN §4.1 p.35).
- On the **Go** command, the peripherals used by the bootloader are *not*
  reset before the jump. The application must reconfigure them, including
  the IWDG prescaler, which the bootloader left at maximum (AN §4.1 p.37).

**Entry conditions: "Pattern 14"** (AN §53.1 p.284; AN Table 2 p.36). The
bootloader runs when any of these holds:
1. BOOT_LOCK = 0, nBOOT1 = 1, **PB8 pin = 1**, nSWBOOT0 = 1;
2. BOOT_LOCK = 0, nBOOT1 = 1, nBOOT0 = 0, nSWBOOT0 = 0;
3. BOOT_LOCK = 0, PB8 pin = 0, nSWBOOT0 = 1, **BFB2 = 1 and neither bank
   holds valid code**;
4. BOOT_LOCK = 0, nBOOT0 = 1, nSWBOOT0 = 0, **BFB2 = 1 and neither bank
   holds valid code**.

What this means:
- **There is no plain empty-flash check on G47x.** The older "Pattern 13"
  has "user flash empty" conditions; G47x uses Pattern 14, which has none
  (AN Table 2 p.36). With BFB2 = 0, the factory setting, a blank or corrupt
  part still boots main flash.
- The only "no BOOT0" entry is the dual-bank-boot path: with **BFB2 = 1**
  the device always starts in the bootloader. The bootloader checks bank 2
  and then bank 1 for a first word (initial SP) that lies inside internal
  SRAM, and jumps to the first valid bank with the bank swap set. If neither
  is valid, it stays in the bootloader (AN §53.2 Figure 70 p.288). On V13.3
  the jump to bank 2 does not work (Table 119).
- **BOOT_LOCK = 1 blocks all four entry patterns** (RM §3.5.6 p.128; AN
  Table 2 p.36).

**What the bootloader would do to TIM's pins if entered by mistake**
(Table 118 crossed with `tim.ioc`). An unexpected entry is not only the
factory PB8 case: **if an option word fails its complement check at load,
the user options are forced to all 1** (RM §3.4.2 p.119). That sets
nSWBOOT0 = 1 and nBOOT1 = 1, and with PB8's 2 kΩ pull-up this is pattern 14
row 1: **the board drops into the bootloader**.

| TIM pin and function | Bootloader role | Effect (inference unless marked) |
|---|---|---|
| PA11 FDCAN1_RX | USB DM, input no pull | Transceiver RXD drives it; CAN traffic toggles a USB data line during detection. Whether the DFU "cable detected" check can fire on that is **UNVERIFIED**. |
| PA12 FDCAN1_TX | USB DP, input no pull | Not driven by the MCU. The ADM3057E TXD has a weak internal pull-up that "holds the transceiver in the recessive state" (ADM pin table p.15, p.24). **CAN1 stays recessive.** |
| PA8 FDCAN1_STB | I2C2_SDA, open drain + pull-up | Internal pull-up (25-55 kΩ, DS p.128) against the ADM3057E STBY internal pull-down (value not given, ADM p.15). The transceiver may go to standby. Either way CAN1 is not disturbed. |
| PB5/PB6 FDCAN2_RX/TX, PB9 FDCAN2_STB | not used | Left in reset state (analog, undriven). **But see the PB6 hazard below.** |
| PA9 / PA10 USART1_TX / RX | USART1, AF push-pull + pull-up | PA9 is driven high (TX idle). Anything sending **0x7F** on USART1 during detection selects the USART1 bootloader. |
| PB12 SPI2_CS (microSD) | SPI2 NSS, input pull-down | CS is weakly pulled low, i.e. the card may be selected. |
| PB13 / PB15 SPI2 SCK / MOSI | SPI2 slave inputs, pull-down | Held low. |
| PB14 SPI2_MISO (card DO) | SPI2 MISO, **driven high** | **Possible contention** with the card's DO if the card drives it. |
| PC10 / PC11 SPI3 SCK / MISO (IMU + mezzanine) | USART3 TX / RX, push-pull + pull-up | PC10 drives SPI3 SCK high; PC11 is pulled up. PD2 SPI3_CS and PC12 MOSI are not touched (floating unless pulled externally). |
| PA2 / PA3 ADC1_IN3 / IN4 | USART2 TX / RX | **PA2 is driven high into an analog input net.** Check what source sits on PA2. PA3 gets a pull-up. |
| PC4 ADC2_IN5 | I2C2_SCL, OD + pull-up | Pull-up loads the analog input. |
| PA4 / PA5 / PA7 (COMP/DAC, GPIO_2) | SPI1 NSS / SCK / MOSI, pull-down | Pulled low. |
| PA6 GPIO_1 (mezzanine) | SPI1 MISO, **driven high** | Contention if the daughterboard drives GPIO_1. |
| PC6 / PC7 CAN LEDs, PC8 heartbeat LED | I2C4 SCL/SDA, I2C3 SCL, OD + pull-up | Active-low LEDs stay off. |
| PC9 Can_Term_1 | I2C3_SDA, OD + pull-up | The termination switch input sees a weak high. Check whether high means termination on. |
| PB8 BOOT0 / I2C1_SCL | not used | Analog after reset. |

**PB6 / FDCAN2 hazard (inference from three documents; bench-test it).**
- `PWR_CR3` resets to 0x0000 8000, so `UCPD1_DBDIS = 0` and the UCPD
  dead-battery pull-downs are enabled after every reset (RM §6.4.3
  p.254-255).
- A high level on PA9 enables a 5.1 kΩ pull-down on **PB6 = FDCAN2_TX**
  (DS Table 12 note 6, p.72).
- The bootloader drives PA9 high as USART1_TX and leaves PB6 unconfigured
  (AN Table 118 p.284). AN2606 does not say whether it sets UCPD1_DBDIS.
- So 5.1 kΩ to ground fights the ADM3057E's *weak* TXD pull-up (ADM p.15).
  That can hold TXD dominant, and the transceiver would drive **dominant on
  CAN2 until its transmit dominant timeout** (t_DT 1175-4000 µs, ADM
  Table 1 p.7) cuts the driver.
- Expect a burst of error frames on CAN2 each time the bootloader starts.
- The application is safe here: `HAL_PWREx_DisableUCPDDeadBattery()` is
  called in `Core/Src/stm32g4xx_hal_msp.c:78`, but only once the application
  runs.
- Fix options: an external pull-up on FDCAN2_TX, BOOT_LOCK = 1, or moving
  FDCAN2_TX off PB6 in a future revision.

---

## 2. Option bytes

`FLASH_OPTR` is at FLASH_R_BASE + 0x20 (CMSIS L466, L1248). Bit map from
CMSIS L5235-5295:

| Bits | Field | Meaning (from the HAL encodings, `stm32g4xx_hal_flash.h`) |
|---|---|---|
| 7:0 | RDP | Read-out protection level |
| 10:8 | BOR_LEV | 0 to 4 give about 1.7, 2.0, 2.2, 2.5, 2.8 V (L293-297) |
| 12 / 13 / 14 | nRST_STOP / nRST_STDBY / nRST_SHDW | Reset generated when entering Stop / Standby / Shutdown |
| 16 | IWDG_SW | 0 = hardware IWDG (starts at reset), 1 = software (L332-333) |
| 17 | IWDG_STOP | **0 = IWDG frozen in Stop**, 1 = runs in Stop (L341-342) |
| 18 | IWDG_STDBY | **0 = frozen in Standby**, 1 = runs (L350-351) |
| 19 | WWDG_SW | Hardware or software WWDG |
| 20 | BFB2 | Dual-bank boot enable (L369-370) |
| 22 | DBANK | **0 = single bank, 128-bit data; 1 = dual bank, 64-bit data** (L378-379) |
| 23 | nBOOT1 | With BOOT0 = 1: 0 boots from SRAM1, 1 boots from system memory (L399-400) |
| 24 | SRAM_PE | 0 = parity on (first 32 KB of SRAM1 + CCM SRAM), 1 = off (L408-409) |
| 25 | CCMSRAM_RST | **0 = CCM SRAM erased on system reset**, 1 = not erased (L416-417) |
| 26 | nSWBOOT0 | 0 = BOOT0 from the `nBOOT0` bit, 1 = BOOT0 from the PB8 pin (L424-425) |
| 27 | nBOOT0 | BOOT0 value used when nSWBOOT0 = 0 |
| 29:28 | NRST_MODE | 01 input only, 10 GPIO, 11 input/output (L442-444) |
| 30 | IRHEN | (not described in the headers) |

- **The CLAUDE.md first-time command checks out.** It writes offset 0x20 with
  mask 0x04000000, which is bit 26 (`nSWBOOT0`), set to 0. That moves BOOT0
  from the PB8 pin to the `nBOOT0` bit.
- **BOR thresholds** (DS Table 19, p.85), falling edge min/typ/max:

  | Level | Falling (V) | Rising (V) |
  |---|---|---|
  | BOR0 | 1.60 / 1.64 / 1.69 | 1.62 / 1.66 / 1.70 |
  | BOR1 | 1.96 / 2.00 / 2.04 | 2.06 / 2.10 / 2.14 |
  | BOR2 | 2.16 / 2.20 / 2.24 | 2.26 / 2.31 / 2.35 |
  | BOR3 | 2.47 / 2.52 / 2.57 | 2.56 / 2.61 / 2.66 |
  | BOR4 | 2.76 / 2.81 / 2.86 | 2.85 / 2.90 / 2.95 |

  Reset temporization after BOR0 is 250 µs typ, 400 µs max (DS p.85). The
  rising/falling pairing in Table 19 had to be untangled from a scrambled
  text extraction, so check it against the PDF page before you rely on it.
  The lowest BOR level is 1.71 V at power-on, and higher thresholds are
  selected by option bytes (§3.11.2, DS p.24).
- **RM0440 confirms the whole bit map above** (RM §3.4.1 p.111-113 and
  §3.7.8 p.138), with these additions: bit 30 is `IRH_EN`, the internal
  reset holder on NRST (holds NRST low until it reaches V_IL); bits 31, 21,
  15 and 11 are reserved and must be kept at 1 when programming;
  `NRST_MODE = 00` also means reset input/output. `DBANK` "can be written
  only when PCROP1/2 is disabled".

### Factory defaults (VERIFIED)
"ST production value: **0xFFEF F8AA**" for the user/RDP option word at
0x1FFF 7800 (RM §3.4.1 p.111). Decoded with the bit map on RM p.111-113:

| Field | Bit(s) | Factory value | Meaning |
|---|---|---|---|
| IRH_EN | 30 | 1 | Reset holder enabled |
| NRST_MODE | 29:28 | 11 | Reset input/output |
| nBOOT0 | 27 | 1 | (used only when nSWBOOT0 = 0) |
| **nSWBOOT0** | 26 | **1** | **BOOT0 taken from the PB8 pin** |
| **CCMSRAM_RST** | 25 | **1** | **CCM SRAM NOT erased on system reset** |
| SRAM_PE | 24 | 1 | **SRAM parity check disabled** |
| **nBOOT1** | 23 | **1** | With BOOT0 = 1, boot from **system memory** |
| **DBANK** | 22 | **1** | **Dual bank, 2 x 256 KB, 2 KB pages** |
| BFB2 | 20 | 0 | Dual-bank boot disabled |
| WWDG_SW | 19 | 1 | Software WWDG |
| IWDG_STDBY | 18 | 1 | IWDG runs in Standby |
| IWDG_STOP | 17 | 1 | IWDG runs in Stop |
| IWDG_SW | 16 | 1 | Software IWDG (not started at reset) |
| nRST_SHDW / STDBY / STOP | 14 / 13 / 12 | 1 / 1 / 1 | No reset on low-power entry |
| **BOR_LEV** | 10:8 | **000** | **BOR level 0, about 1.7 V** |
| RDP | 7:0 | 0xAA | Level 0 |

Other factory words: PCROP1 start 0xFFFF FFFF, PCROP1 end 0x00FF 0000,
WRP1A and WRP1B 0xFF00 FFFF (RM p.113-114), i.e. no WRP or PCROP.

- **The CLAUDE.md first-time-setup note is now verified.** Factory
  `nSWBOOT0 = 1` and `nBOOT1 = 1`, so PB8 high (2 kΩ pull-up) boots the
  system-memory bootloader, exactly the 0x1FFF_xxxx symptom. After the
  CLAUDE.md command clears nSWBOOT0, BOOT0 comes from `nBOOT0`, which is 1
  at the factory, so the part boots from main flash.
- **BOR level 0 is the factory setting.** A 3V3 design that wants a higher
  brown-out threshold must program `BOR_LEV` itself.

### Boot mode selection (VERIFIED, RM §2.6.1 Table 5 p.91)

| BOOT_LOCK | nBOOT1 | nBOOT0 | PB8 | nSWBOOT0 | Boots from |
|---|---|---|---|---|---|
| 1 | x | x | x | x | Main flash (forced) |
| 0 | x | x | 0 | 1 | Main flash |
| 0 | x | 1 | x | 0 | Main flash |
| 0 | 0 | x | 1 | 1 | SRAM1 |
| 0 | 0 | 0 | x | 0 | SRAM1 |
| 0 | 1 | x | 1 | 1 | System memory |
| 0 | 1 | 0 | x | 0 | System memory |

- **No empty-flash check appears in RM0440.** Table 5 has no row for it and
  the word does not occur in the boot or flash chapters. Whether the G474
  bootloader itself has an empty-check is AN2606 material: **UNVERIFIED**.
  **Third pass: VERIFIED that there is none.** G47x uses AN2606 activation
  "Pattern 14", which has no "user flash empty" condition; the only
  code-validity check runs when BFB2 = 1 (§1, AN Table 2 p.36, AN Figure 70
  p.288).
- With `BFB2 = 1` and a main-flash boot configuration, the part actually
  boots the system-memory bootloader, which jumps to bank 2; system memory
  stays aliased at 0x0000 0000, so VTOR must be set (RM §2.6.1 p.91-92).
- `BOOT_LOCK` (in `FLASH_SEC1R`) forces boot from main flash regardless of
  the other bits; it can be changed only at RDP 0, or at RDP 1 together with
  a regression to 0 and a full mass erase (RM §3.5.6 p.128).

### Option-byte programming and loading (VERIFIED, RM §3.4.2 p.118-119)
- Unlock `FLASH_CR` (KEY1 0x45670123, KEY2 0xCDEF89AB), then `OPTLOCK`
  (OPTKEY1 0x08192A3B, OPTKEY2 0x4C5D6E7F). A wrong key sequence locks
  `FLASH_CR` until the next reset and raises a HardFault (RM §3.3.5 p.103).
- Write `FLASH_OPTR` etc., set `OPTSTRT`, wait for BSY to clear. Any change
  erases both option pages and reprograms all of them.
- New values take effect only on option-byte loading: setting
  `OBL_LAUNCH` (which **generates a system reset**, flagged as OBLRSTF), or a
  power reset (BOR, or exit from Standby/Shutdown). A plain NRST or IWDG
  reset does **not** reload option bytes.
- If an option word and its complement mismatch at load, `OPTVERR` is set and
  safe values are forced: user options all 1 except **BOR_LEV = 000**; WRP
  none; **RDP Level 1**; PCROP "all memory protected".
- **Debug freeze is not an option byte.** IWDG and WWDG freeze under debug is
  `DBGMCU_APB1FZR1.DBG_IWDG_STOP` (bit 12) and `DBG_WWDG_STOP` (bit 11)
  (CMSIS L3165-3168). RM0440 confirms: 1 = counter clock stopped when the
  core is halted. The register is reset only by power-on reset; **a system
  reset does not affect it** (RM §47.16.4 p.2109-2110).

---

## 3. Flash (512 KB device)

- **Size and banks.** 512 KB with ECC, "two banks read-while-write" (DS p.1).
  The block diagram shows "FLASH 2 x 256 KB" (DS Figure 1, p.17). Single or
  dual bank mode, with RWW in dual bank mode only (§3.4, DS p.19).
- **Page size depends on DBANK:**
  - **Dual bank (DBANK = 1)**: 2 KB pages, 128 pages per bank, 256 KB per bank.
  - **Single bank (DBANK = 0)**: 4 KB pages, 128 pages.

  Sources: DS Table 47 uses "One page (2 Kbytes)" and "One bank (256 Kbyte)"
  (DS p.124). The HAL gives `FLASH_PAGE_SIZE` 0x800, `FLASH_PAGE_SIZE_128_BITS`
  0x1000 ("4 KB"), `FLASH_PAGE_NB` 128 for 512 KB, and
  `FLASH_BANK_SIZE = FLASH_SIZE/2` (HAL `stm32g4xx_hal_flash.h` L871-885).
  `FLASH_CR.PNB` is 7 bits, so 128 page numbers, and `BKER` (bit 11) selects
  the bank (CMSIS L5166-5168).
- **RM0440 confirms both layouts** (RM §3.3.1 p.95-96, Tables 7 and 8):
  - DBANK = 1: two banks of 256 KB, 128 pages of 2 KB each, page = 8 rows of
    256 bytes, 64-bit (+8 ECC) read. Bank 1 is 0x0800 0000-0x0803 FFFF,
    bank 2 is 0x0804 0000-0x0807 FFFF. Bank and mass erase available.
  - DBANK = 0: one bank of 512 KB, 128 pages of **4 KB**, page = 8 rows of
    512 bytes, 128-bit (+2 x 8 ECC) read. Only page erase and mass erase;
    setting only one of MER1/MER2 gives PGSERR (RM §3.3.6 p.104).
  - **Factory default is DBANK = 1, dual bank** (production value, §2).
- **Changing DBANK** must be done from SRAM or the bootloader, with caches
  and prefetch off and flushed, all WRP cleared; after OBL, **mass erase**
  and reprogram in the new mapping (RM §3.4.2 p.119-120). Every flash
  address, the linker script and any data-page scheme change with it.
- **Program unit.** 64-bit double word, which is the ECC granularity
  (Table 47 "64-bit programming time", DS p.124). In single bank mode the data
  width is 128 bits (HAL L378).
- **Times** (Table 47, DS p.124, typ / max; the raw text extraction was used to
  pair the columns):

  | Operation | Typ | Max |
  |---|---|---|
  | 64-bit program | 81.7 µs | 83.35 µs |
  | Row (32 double words), normal | 2.61 ms | 2.7 ms |
  | Row, fast | 1.91 ms | 1.95 ms |
  | 2 KB page program, normal | 20.91 ms | 21.34 ms |
  | 2 KB page program, fast | 15.29 ms | 15.6 ms |
  | **2 KB page erase** | **22.02 ms** | **24.47 ms** |
  | 256 KB bank program, normal | 2.68 s | 2.73 s |
  | 256 KB bank program, fast | 1.96 s | 2 s |
  | Mass erase (one or two banks) | 22.13 ms | 24.6 ms |

  Supply current is 3.5 mA average for write and for erase. Peaks are 7 mA for
  6 µs (write) and 7 mA for 67 µs (erase).
- **Endurance** (Table 48, DS p.124): **10 kcycles** min at TA = -40 to
  +105 °C. Retention after 1 kcycle: 30 years at 85 °C, 15 years at 105 °C,
  7 years at 125 °C. After 10 kcycles: 30 years at 55 °C, 15 at 85 °C, 10 at
  105 °C. The DS gives **no endurance figure above 105 °C**, although the part
  is rated to 125 °C.
- **Does an erase stall execution? Yes, unless the code runs from the other
  bank (VERIFIED).** RM0440 Rev 9, §3.3.5 p.103:
  > "An on-going flash memory operation does not block the CPU as long as the
  > CPU does not access the same flash memory bank. [...] during a
  > program/erase operation to the flash memory, any attempt to read the same
  > flash memory bank stalls the bus. The read operation proceeds correctly
  > once the program/erase operation has completed."

  RWW "is available only when the DBANK option bit is set" (RM §3.3.8
  p.108). Consequences per configuration:

  | Configuration | Code/vectors fetched from | Effect of a page erase |
  |---|---|---|
  | Single bank (DBANK = 0) | Flash (anywhere) | **CPU stalls** for the whole erase: one 4 KB page. The DS times are for 2 KB pages; no 4 KB figure is given. |
  | Dual bank, same bank | The bank being erased | **CPU stalls** for the whole erase, up to 24.47 ms per 2 KB page (DS p.124) |
  | Dual bank, other bank | The bank not being erased | **No stall**: the CPU runs normally |
  | Any | SRAM / CCM SRAM only | No stall (no flash read) |

  - **Interrupts:** the RM says nothing interrupt-specific. The stall is a bus
    stall on reads of that bank, so any interrupt whose vector table, handler
    code or literal pools are in the busy bank also waits the full erase time.
    That is an inference from the quoted rule, not a separate ST statement.
    To keep interrupt latency during an erase, VTOR, the handlers and
    everything they touch must be in SRAM or in the other bank. DMA reads of
    the busy bank stall the same way.
  - **Write-while-write is not allowed:** you cannot erase one bank while
    programming the other (RM §3.3.8 p.108).
  - Any write to `FLASH_CR` while BSY is set also stalls the AHB until BSY
    clears (RM §3.3.5 p.103).
  - **Fast programming:** in single bank mode it may be done only from RAM or
    the bootloader, and it needs a mass erase first; if a read happens in the
    same bank during fast programming, the programming is **aborted with no
    error flag** (RM §3.3.7 p.106).
  - **Design consequence for TIM:** the factory DBANK = 1 setting is the one
    to keep. Link the application into bank 1 and put any logged or
    configuration data in bank 2 pages; then erases do not stall the VCU
    torque path.
  - **Errata that apply to this plan** (§7.1):
    - ES §2.2.2: the D-cache can be corrupted during read-while-write, so
      disable and reset it around every flash write (the HAL does).
    - ES §2.2.3: after a reset or power-down mid-operation, re-erase the
      page.
- **Erase and program sequence constraints (VERIFIED, RM §3.3.5-3.3.7
  p.103-108):**
  - Flash program/erase is **only possible in voltage range 1**
    (`PWR_CR1.VOS = 01`) (p.103).
  - Unlock with KEY1/KEY2; a wrong sequence locks `FLASH_CR` until reset and
    raises a HardFault (p.103).
  - Page erase: check BSY clear; clear all old error flags (else PGSERR); set
    PER, PNB and (dual bank only) BKER (BKER must stay 0 in single bank); set
    STRT; wait for BSY to clear (p.103). Erasing a WRP/PCROP page sets
    WRPERR and aborts (p.104).
  - Program: only a 64-bit double word, aligned, written as two 32-bit words;
    byte/half-word writes give SIZERR, misalignment gives PGAERR. A location
    can be programmed again only with all zeros, otherwise PROGERR
    (p.104-105).
  - HSI16 is switched on automatically while STRT/PG/FSTPG is set (p.104-105).
  - **A reset during a flash operation leaves the flash contents undefined**
    (p.102). Any data-logging scheme needs a validity marker written last.
  - After erasing data that may be cached, flush the I/D caches (disable,
    then ICRST/DCRST) (p.108).
  - ECC: single-bit errors are corrected (ECCC, optional interrupt);
    double-bit errors set ECCD and raise an **NMI** (RM §3.3.2 p.97).
- **OTP.** 1 KB (128 double words), in Bank 1 only, write-once (DS p.19).

---

## 4. FDCAN

- **Instances and RAM.** FDCAN1, FDCAN2 and FDCAN3 meet ISO 11898-1 and CAN FD
  v1.0. They share a **3 KB message RAM** holding the filters, RX FIFOs, TX
  event FIFOs and TX buffers (§3.33, DS p.45).
- **Fixed per-instance layout** (HAL `stm32g4xx_hal_fdcan.c` L211-235). The
  G4 FDCAN message RAM has a hard-wired layout; RM0440 confirmation is
  pending.

  | Item | Count per instance |
  |---|---|
  | Standard ID filters | **28** |
  | Extended ID filters | **8** |
  | RX FIFO 0 | **3** elements |
  | RX FIFO 1 | **3** elements |
  | TX event FIFO | **3** elements |
  | TX FIFO/queue buffers | **3** |

  Each RX/TX element is 72 bytes (18 words, so the payload always fits 64
  bytes). That is 848 bytes per instance and 2544 bytes for all three, which
  fits the 3 KB. `RXGFC.LSS` is a 5-bit field and `RXGFC.LSE` is 4-bit
  (CMSIS L4914-4918).
- **RM0440 confirms the fixed layout (VERIFIED).** "A 0.8-Kbyte message RAM
  per FDCAN instance" (RM §44.1 p.1951); up to 212 words per instance, each
  instance 0x350 bytes, laid out as 28 standard filters, 8 extended filters,
  RX FIFO 0 (3 elements), RX FIFO 1 (3), TX event FIFO (3), TX buffers (3)
  (RM §44.3.6 Figure 670 p.1967). Features list: "Two receive FIFOs of three
  payloads each", "Configurable transmit FIFO/queue of three payloads"
  (RM §44.2 p.1953). `LSS` 1-28 (values > 28 read as 28) and `LSE` 1-8
  (> 8 read as 8) (RM §44.4.19 p.2002).
- **Filters (RM §44.3.6 p.1967-1968):** each list runs from element 0 and
  stops at the **first match**. An element can be a range, one or two exact
  IDs, or a classic ID/mask, and can accept, reject, or flag high priority.
- **Default for non-matching frames is ACCEPT into RX FIFO 0.**
  `RXGFC` resets to 0, and `ANFS = 00` / `ANFE = 00` mean "accept in Rx FIFO
  0" (RM §44.4.19 p.2001-2002). Firmware that wants only its filtered IDs
  must set ANFS/ANFE to reject.
- **There are no dedicated RX buffers.** The RX path is only the two 3-deep
  FIFOs, so at high bus load firmware must drain them quickly (ISR or DMA-free
  copy).
- **RX FIFO full behaviour (VERIFIED, RM §44.3.6 p.1972):**
  - **Blocking mode** (`RXGFC.FnOM = 0`, **the reset default**): when full,
    no more frames are written until one is read and acknowledged; a frame
    arriving while full is **discarded** and `RFnL` (message lost) is set in
    `RXFnS` and `IR`. The oldest frames are kept.
  - **Overwrite mode** (`FnOM = 1`, F0OM is bit 9, F1OM bit 8): the next
    accepted frame **overwrites the oldest**; put and get indices both
    advance. While full, read starting at get index + 1, because the hardware
    may be writing the get-index element as you read it. The newest frames
    are kept.
  - Either way, acknowledge by writing the last element read to `RXFnA.FnA`.
  - A frame with a CAN error after acceptance does not advance the put index
    but may partly overwrite the FIFO element (RM p.1968).
- **TX modes (VERIFIED, RM §44.3.6 p.1973-1975):**
  - **FIFO mode** (`TXBC.TFQM = 0`): sent in the order written, from the get
    index, so frames with the same ID leave in order.
  - **Queue mode** (`TFQM = 1`): **sent lowest message ID first (highest
    priority)**. Standard IDs are compared with bits [28:18] of extended IDs;
    equal IDs go lowest buffer number first. The Tx scan for the
    highest-priority pending request runs whenever `TXBRP` changes or a
    transmission starts (p.1973).
  - Cancellation (`TXBCR`) is meant for queue mode, not FIFO mode. A
    cancellation that hits a frame already on the bus lets it finish; if that
    attempt fails it is not retried (p.1975).
  - Transmit pause (`CCCR.TXP`) inserts two bit times after each successful
    frame; off by default (p.1973).
  - TX event FIFO: 3 elements; when full, new events are dropped and `TEFL`
    is set (p.1975).
- **Error counters, `FDCAN_ECR`** (CMSIS L4643-4652; ranges from HAL
  `stm32g4xx_hal_fdcan.h` L378-396):

  | Field | Bits | Range / meaning |
  |---|---|---|
  | TEC | 7:0 | 0 to 255 |
  | REC | 14:8 | 0 to 127 |
  | RP | 15 | REC reached 128 |
  | CEL | 23:16 | Error-logging count, saturates at 255, then raises the ELO flag |

- **Protocol status, `FDCAN_PSR`** (CMSIS L4657-4687; meanings from HAL L320-369):

  | Field | Bits | Meaning |
  |---|---|---|
  | LEC | 2:0 | Last error code |
  | ACT | 4:3 | Activity |
  | EP | 5 | Error passive |
  | EW | 6 | Warning: a counter reached 96 |
  | BO | 7 | Bus-off |
  | DLEC | 10:8 | Data-phase last error code |
  | RESI / RBRS / REDL | 11 / 12 / 13 | ESI, BRS and FDF of the last received frame |
  | PXE | 14 | Protocol exception |
  | TDCV | 22:16 | Transmitter delay compensation value |

  The interrupt flags are `IR.EP` (17), `IR.EW` (18), `IR.BO` (19), `PEA`
  (21), `PED` (22) and `ARA` (23) (CMSIS L4751-4769).
- **RM0440 confirms the ECR and PSR maps above** (RM §44.4.12-44.4.13
  p.1992-1994) and adds read side effects that matter for a status task:
  - `ECR.CEL` is **cleared on read**; at 0xFF the next error sets `IR.ELO`.
  - `PSR.LEC` and `PSR.DLEC` are **set to 7 ("no change") on read**, and
    `PXE`, `REDL`, `RBRS`, `RESI` are **cleared on read**. Reading `PSR` in
    two places loses information; read it once and fan the value out.
  - `PSR` reset value is 0x0000 0707.
  - With `CCCR.ASM` (restricted operation) set, TEC/REC do not count but CEL
    does.
- **Bus-off (VERIFIED):**
  - **Entry:** "The bus-off state is reached when TEC[7:0] is greater than
    255", shown by `PSR.BO` (RM §44.3.5 p.1966) and the `IR.BO` interrupt.
  - **The peripheral sets INIT itself and stays there.** "If the device
    enters bus-off, it sets the INIT bit of its own, stopping all bus
    activities." Software initialisation is entered "by software, by a
    hardware reset, or by entering bus-off state" (RM §44.3.4 p.1957;
    §44.4.13 p.1994). While INIT is set, `FDCAN_TX` is recessive and nothing
    is sent or received. **Nothing happens until software clears INIT.**
  - **Recovery:** "Once INIT has been cleared by the CPU, the device waits for
    **129 occurrences of bus-idle (129 x 11 consecutive recessive bits)**
    before resuming normal operation. At the end of the bus-off recovery
    sequence, the error management counters are reset." Each 11-recessive
    sequence writes a Bit0 error (5) to `PSR.LEC`, and `ECR.REC` counts the
    sequences, so firmware can watch progress (RM §44.4.13 p.1994).
  - The sequence "cannot be shortened by setting or clearing the INIT bit".
    In INIT the core does not monitor RX, so recovery only proceeds in normal
    mode (RM §44.3.5 p.1966).
  - **RM0440 inconsistency:** §44.3.5 p.1966 and its Figure 669 say **128**
    x 11 recessive bits; §44.4.13 p.1994 says **129** x 11 after INIT is
    cleared. p.1966 attributes the 128 figure to "the CAN standard" (the
    standard itself was not read here). Budget for 129 x 11 = 1419 bit times
    (about 2.8 ms at 500 kbit/s) after INIT is cleared.
  - Clearing INIT also costs one more bus-idle (11 recessive bits) of
    integration (RM §44.3.4 p.1957). The INIT bit crosses clock domains:
    read it back before writing it again (RM §44.4.6 p.1988).
  - The timeout counter stops while INIT is set, e.g. in bus-off (RM p.1965).
  - Also relevant: `CCCR.DAR` (bit 6) disables automatic retransmission
    (RM §44.4.6 p.1988), and Restricted Operation mode (`CCCR.ASM`).
- **Classic-CAN-only operation (VERIFIED, RM §44.3.4 p.1958-1959 and
  Table 410 p.1973):**
  - `CCCR.FDOE = 0` (the reset value; `CCCR` resets to 0x0000 0001, INIT
    set) makes the node classic only: FDF/BRS in TX elements are ignored and
    every frame goes out as classic CAN.
  - **With FDOE = 0, a received CAN FD frame is treated as an error and the
    node transmits an error frame.** A classic-only TIM channel will destroy
    FD traffic on a mixed bus. Only set FDOE = 0 on buses that never carry FD
    frames.
  - `CCCR.NISO` selects ISO 11898-1 (0, default) or Bosch FD v1.0 framing;
    FDOE and BRSE can be changed only with INIT and CCE both set
    (RM p.1959, p.1987).
  - On an FD-enabled node, a frame with res recessive is a protocol
    exception: `PSR.PXE` is set and the node re-integrates, unless
    `CCCR.PXHD = 1`, in which case it is a form error (RM p.1959).

---

## 5. IWDG

- **Counter and clock.** 12-bit down-counter with an 8-bit prescaler, clocked
  by the 32 kHz LSI. It keeps running in **Stop and Standby**. Hardware or
  software start is set by option byte. "The counter can be frozen in debug
  mode" (§3.24.6, DS p.38).
- **Timeout range** (Table 85, DS p.170, at LSI = 32 kHz). Minimum is
  RL = 0x000, maximum is RL = 0xFFF:

  | Prescaler | Min | Max |
  |---|---|---|
  | /4 | 0.125 ms | 512 ms |
  | /8 | 0.25 ms | 1024 ms |
  | /16 | 0.5 ms | 2048 ms |
  | /32 | 1.0 ms | 4096 ms |
  | /64 | 2.0 ms | 8192 ms |
  | /128 | 4.0 ms | 16384 ms |
  | /256 | 8.0 ms | 32768 ms |

  There is always one full RC period of uncertainty, from APB/LSI phasing
  (note 2).
- **LSI accuracy** (Table 45, DS p.122). At VDD = 3.0 V and TA = 30 °C the LSI
  is 31.04 to 32.96 kHz. Over VDD = 1.62 to 3.6 V and TA = -40 to 125 °C it is
  **29.5 to 34 kHz**. Size the timeout for a 34 kHz LSI (it expires early) and
  the kick period for a 29.5 kHz LSI, a spread of about -13 % to +6 %.
- **Basic operation (VERIFIED, RM §35.3.2 p.1557):** writing 0xCCCC to
  `IWDG_KR` starts it, counting down from 0xFFF; at 0x000 it resets the
  device. 0xAAAA reloads from `IWDG_RLR`. 0x5555 unlocks `PR`, `RLR` and
  `WINR`; any other key relocks them (RM §35.3.5 p.1558). Prescaler codes
  000-110 are /4../256, and 111 is also /256 (RM §35.4.2 p.1560). The IWDG
  logic sits in the VDD domain and stays functional in Standby (RM Figure 529
  note, p.1556).
- **Window mode (VERIFIED, RM §35.3.2 p.1557):**
  - "If the reload operation is performed while the counter is **greater
    than** the value stored in [IWDG_WINR], a reset is provided." So a
    refresh is legal only when counter <= WIN; refreshing too early resets
    the part, as does reaching 0.
  - `IWDG_WINR` resets to **0x0000 0FFF**, which disables the window.
  - Writing a new window value **also reloads the counter** from RLR.
  - Configure in this order: 0xCCCC, 0x5555, PR, RLR, wait for
    `IWDG_SR = 0`, then write WINR (which does the first refresh).
  - In hardware-watchdog mode the window also applies (RM §35.3.3 p.1558).
- **Cannot be stopped (VERIFIED):** "Once running, the IWDG cannot be
  stopped" (RM §35.3.2 p.1557). Only a reset stops it, and with
  `IWDG_SW = 0` it restarts by itself at power-on (RM §35.3.3 p.1558).
- **Stop and Standby (VERIFIED):** `IWDG_STOP` and `IWDG_STDBY` choose
  whether the counter runs in Stop and Standby; if it runs, it can wake the
  device (RM §35.3.4 p.1558). **Factory default is 1/1, i.e. running** (§2).
  Standby exit is a power reset, but IWDG is outside the VCORE domain and is
  not reset by it (RM §7.1.1 p.270).
- **Debug (VERIFIED):** with the core halted, the counter runs or stops per
  `DBGMCU_APB1FZR1.DBG_IWDG_STOP` (RM §35.3.6 p.1558; §47.16.4 p.2110). The
  bit resets to 0 (runs) at POR and is **not changed by system reset**. It
  must be set by firmware or the debugger to stop the IWDG resetting the
  part at a breakpoint.
- **Hardware-start mode.** `IWDG_SW = 0` starts the IWDG automatically at
  power-on (RM §35.3.3 p.1558); the factory value is 1, software start.
- **GPIOs on an IWDG reset (VERIFIED):** an IWDG event is a system reset
  (RM §7.1.2 p.270); all registers including GPIO return to reset values,
  i.e. analog/high-impedance (RM §9.4 p.355-356, see §1). NRST is pulsed low
  for at least 20 µs in the factory NRST mode. `RCC_CSR.IWDGRSTF` records it.

---

## 6. Reset causes and retained memory

- **Reset-cause flags, `RCC_CSR`** (CMSIS L12855-12876):

  | Flag | Bit | Cause |
  |---|---|---|
  | RMVF | 23 | Write 1 to clear all flags |
  | OBLRSTF | 25 | Option-byte load |
  | PINRSTF | 26 | NRST pin |
  | BORRSTF | 27 | BOR / power-on |
  | SFTRSTF | 28 | Software |
  | IWDGRSTF | 29 | IWDG |
  | WWDGRSTF | 30 | WWDG |
  | LPWRRSTF | 31 | Illegal low-power entry (with the nRST_xxx bits) |

  There is no firewall-reset flag on G474. The HAL comment at
  `stm32g4xx_hal_rcc.h` L3193 mentions `RCC_FLAG_FWRRST`, but the G474 headers
  do not define it. Internal resets (IWDG, software and others) also pulse
  NRST, so PINRSTF is likely set alongside them; that is **UNVERIFIED**
  (RM0440 RCC "reset" section). Firmware should decide the cause in priority
  order and then write RMVF.
- **RM0440 confirms the bit map** (RM §7.4.28 p.327-328) and adds:
  - `RCC_CSR` reset value is **0x0C00 0000**: after power-on, **BORRSTF and
    PINRSTF are both set**.
  - The flags are **reset by power reset only**; a system reset leaves them,
    so they accumulate across resets until software writes RMVF.
  - IWDGRSTF, WWDGRSTF, SFTRSTF, OBLRSTF and LPWRRSTF descriptions match the
    table above.
  - Whether PINRSTF is also set by an internal reset that is driven out on
    NRST: RM0440 shows all internal sources going through the NRST pulse
    generator (RM §7.1.2 Figure 16 p.271) but never says which flags result.
    **Still UNVERIFIED**; check on a board (trigger an IWDG reset and read
    `RCC_CSR`). Decode IWDG/WWDG/SFT/OBL/LPWR first, BOR next, PIN last.
- **Reset types (RM §7.1 p.270-272):**
  - *Power reset* (POR/BOR, exit from Standby or Shutdown): all registers
    reset except the RTC domain. On Standby exit only the VCORE domain is
    reset; RTC, WKUP, IWDG and Standby control are kept.
  - *System reset* (NRST, WWDG, IWDG, software, low-power security, OBL,
    BOR): all registers to reset values unless stated otherwise.
  - *RTC domain reset*: `RCC_BDCR.BDRST`, or **VDD or VBAT power-on after
    both were off**. Resets only LSE, RTC, backup registers and `RCC_BDCR`.
- **SRAM layout** (§3.5, DS p.20; CMSIS L1134-1152):

  | Region | Size | Address | Notes |
  |---|---|---|---|
  | SRAM1 | 80 KB | 0x2000_0000 | First 32 KB has parity |
  | SRAM2 | 16 KB | 0x2001_4000 | Can be retained in Standby (`PWR_CR3.RRS`, CMSIS L10820) |
  | CCM SRAM | 32 KB | 0x1000_0000, aliased at 0x2001_8000 | Has parity; write-protect in 1 KB granules |

- **What survives a system reset (NRST, software, IWDG):**
  - ~~CCM SRAM is erased on system reset by default~~ **(corrected)**.
    `CCMSRAM_RST = 0` erases CCM SRAM on every system reset, but the **factory
    value is 1, "not erased"** (RM §3.4.1 p.111-112). CCM SRAM is erased
    only if someone programs the option to 0, by software via
    `SYSCFG_SCSR.CCMER`, or on an RDP 1 -> 0 regression (RM §2.4.4 p.90;
    §3.5.1 p.122-124). A CCM SRAM crash record therefore survives a system
    reset on a part with factory options. Check `FLASH_OPTR` bit 25 at boot
    if you rely on it.
  - **SRAM1 and SRAM2 across a system reset:** RM0440 defines **no
    mechanism that erases them on reset**. The only reset-linked erase it
    describes is the CCM SRAM one; SRAM content is lost only where power is
    removed: Standby (except SRAM2 with `PWR_CR3.RRS = 1`), Shutdown, or VDD
    loss (RM §6.1.4 p.231-232). RM0440 has no sentence saying "SRAM1/2 are
    retained across a system reset"; retention is the consequence of VCORE
    staying powered and no erase being defined. Treat it as **verified by
    omission** and confirm on a board before a safety argument depends on it.
    The C start-up code zeroes `.bss` and copies `.data`, so a crash record
    needs its own `NOLOAD` section.
  - Two later findings bear on this:
    - ES §2.2.6 (third pass): the first SRAM write after a system reset can
      be lost, so a dummy read is needed at reset (§7.1).
    - The system bootloader uses **0x2000 0000-0x2000 3FFF** (AN Table 118
      p.284), so a crash record there is lost if the board passes through
      the bootloader.
  - **Parity (VERIFIED):** on category 3, parity covers the first 32 KB of
    SRAM1 and all of CCM SRAM; SRAM2 has none. A parity error raises an
    **NMI** (and can drive timer break inputs). ST advises initialising all
    of SRAM at start-up when parity is on, to avoid errors when reading
    never-written locations (RM §2.4.1 p.89). The **factory value
    `SRAM_PE = 1` means parity is off.**
  - Standby and Shutdown lose SRAM and registers, except SRAM2 in Standby with
    RRS, and the RTC domain (DS p.25).
- **Backup registers (TAMP_BKPxR).** 32 x 32-bit. They are retained in all
  low-power modes and in VBAT mode, and are **not reset by a system or power
  reset**, nor on wake from Standby or Shutdown. Any tamper detection erases
  them (§3.26, DS p.39). They sit at TAMP_BASE = APB1 + 0x2400 (CMSIS L1175).
  - **Usable without VBAT?** While VDD is present, the backup domain is fed
    from VDD through the power switch (§3.11.1 and Figure 16, DS p.23, p.81).
    The registers should therefore survive any reset that does not remove VDD.
    With VBAT unconnected, they are lost when VDD drops, and their behaviour
    at VDD power-up with VBAT floating is **UNVERIFIED**.
  - **RM0440 confirms (VERIFIED):** "not reset by system reset or when the
    device wakes up from Standby mode"; reset by a tamper event (unless
    TAMPxNOER or TAMPxMSK is set) and by RDP 1 -> 0 (RM §38.3.4 p.1613); 32
    registers on category 3 (RM §38.1 p.1610). A BOR resets everything
    except the RTC domain (RM §7.1.1 p.270), so they also survive a
    brown-out while VDD stays above the switch threshold.
  - **They are reset by an RTC domain reset, which includes "VDD or VBAT
    power on, if both supplies have previously been powered off"** (RM
    §7.1.3 p.272). With VBAT not powered, every VDD power cycle is such a
    power-on, so the contents are lost (inference from that rule).
  - **Write access (VERIFIED):** after a system reset the TAMP and backup
    registers are write-protected; enable the PWR clock, then set
    `PWR_CR1.DBP` (RM §6.1.3 p.231; §38.3.3 p.1612).
  - Practical read: the backup registers are a fair place for a
    reset-reason or crash record across a watchdog reset. Do not rely on them
    across power cycles.

---

## 7. Errata (ES0430 Rev 10, VERIFIED)

**Scope.** ES0430 Rev 10 (November 2026) covers STM32G474RE among the
G471/473/474/483/484 part numbers (ES Table 1 p.1). It covers three silicon
revisions (ES Table 2 p.1):

| Marking | `DBGMCU_IDCODE.REV_ID` |
|---|---|
| Z | 0x2001 |
| Y | 0x2002 |
| X | 0x2003 |

**The TIM board's revision is not known yet: read `REV_ID` over SWD.** Status
codes: A = workaround available, P = partial workaround, N = no workaround,
"-" = not applicable (ES Table 3 p.2-4). Unless noted, an item applies to
**all three revisions**.

**Peripherals with no erratum at all in ES0430:** IWDG, WWDG, GPIO, the FLASH
erase/program stall, FDCAN bus-off, and timer input capture. A full-text
search for IWDG, WWDG, watchdog, bootloader, BOOT0, nSWBOOT0, DBANK and BFB2
found nothing except the ADC analog watchdog.

### 7.1 Errata that need a change in TIM firmware or design (ranked)

1. **ES §2.2.6 SRAM write error (A), p.10.**
   - *Condition:* in rare cases a system reset at a critical instant upsets
     the SRAM state machine; the first access after reset restores it, but
     **if that first access is a write, the write is lost**.
   - *Workaround:* right after reset, do a dummy **read** of each 32 KB SRAM
     instance used: 0x2000 0000, 0x2000 8000, 0x2001 0000, 0x2001 4000
     (SRAM2), 0x1000 0000 (CCM SRAM). For instances with parity enabled, use
     a dummy variable initialisation instead.
   - *TIM:* `startup_stm32g474xx.s` `Reset_Handler` sets SP and calls
     `SystemInit` with no dummy reads. **The startup code needs the
     workaround.** This matters for IWDG resets and for any reset-surviving
     crash record (§6).
2. **ES §2.7.9 Wrong ADC result if the conversion comes more than 1 ms after
   the previous conversion or calibration (A), p.17.**
   - *Workaround:* do two consecutive conversions and keep only the second.
   - *TIM:* `App/Src/vbat.c` does one software-triggered conversion every
     500 ms (`VBAT_SAMPLE_PERIOD_MS`), so **every VBAT reading is in the
     erratum's window**. Add a discarded first conversion.
3. **ES §2.2.2 Data cache corrupted during flash read-while-write (A), p.8.**
   - *Condition:* dual-bank mode, data cache on, a read from one bank while
     writing the other.
   - *Workaround:* clear DCEN before any flash write, then set and clear
     DCRST before re-enabling DCEN.
   - *TIM:* this is exactly the bank-2 data-storage plan in §3. The HAL
     already does it: `HAL_FLASH_Program` and `HAL_FLASHEx_Erase` disable the
     D-cache, and `FLASH_FlushCaches` resets it
     (`stm32g4xx_hal_flash.c` L192-195, `stm32g4xx_hal_flash_ex.c` L620-641).
     **Use only the HAL flash API, or copy the sequence.**
4. **ES §2.2.3 FLASH_ECCR corrupted after a reset or power-down during
   program/erase (A), p.8.**
   - *Condition:* a reset or power-down during a program or erase, followed
     by a read of the same location, may corrupt `FLASH_ECCR`.
   - *Workaround:* erase the affected page(s).
   - *TIM:* the logging scheme must detect an interrupted write (validity
     marker, §3) and **re-erase that page**, not just skip it.
5. **ES §2.2.5 Backup domain content corrupted after a missed power-on reset
   (A), p.9.**
   - *Condition:* if the backup-domain supply falls into a window between
     100 mV and 700 mV during power-off and then rises again, the backup
     domain reset may be missed; backup registers and RCC_BDCR hold
     unpredictable values, which can even drive LSCO on PA2.
   - *Workaround:* hold VDD and VBAT below 100 mV for more than 200 ms, or at
     boot, if `BORRSTF` is set, reset the backup domain in software
     (`RCC_BDCR` = 0x0001 0000, read it back, write 0, then RMVF).
   - *TIM:* with VBAT floating (§1) the backup supply decays in an
     uncontrolled way. Anything kept in TAMP backup registers must be guarded
     (CRC), or the domain reset on every BOR.
6. **ES §2.18.1 / §2.18.2 SPI BSY flag unreliable (A), p.29-30.**
   - *Conditions:* BSY may stay high after SPI disable in master transmit
     mode with TXE = 0, in master receive-only mode, or in slave mode when
     NSS is removed. It may also stay high at the end of a slave transfer.
   - *Workaround (TIM uses SPI2 and SPI3 as master):* disable SPI only when
     TXE = 1 and BSY = 0; in receive-only mode ignore BSY. Do not rely on BSY
     alone as end-of-transfer, which matters for the spi3Mutex hand-off
     between the IMU and the mezzanine.
7. **ES §2.7.11 One ADC instance may degrade another's accuracy (A),
   p.17-18.**
   - *Condition:* conversions on different ADC instances overlap.
   - *Workaround:* do not overlap them, or use a supported clock setup. For
     ADC1/ADC2 in normal mode, a synchronous clock with any prescaler is
     supported. With a prescaler above 1, a hardware trigger's clock
     prescaler must be a multiple of the ADC prescaler.
   - *TIM:* ADC1 and ADC2 are `ADC_CLOCK_SYNC_PCLK_DIV4`, independent mode
     (`Core/Src/main.c` L261, L283, L328): a supported configuration. If
     ADC1/2 move to timer triggers, the trigger timer's prescaler must be a
     multiple of 4.
8. **ES §2.1.2 VDIV/VSQRT may not complete when very short ISRs are used (A,
   Arm 776924), p.5-6.**
   - *Condition:* FPU on, lazy stacking on (the default), and an ISR with no
     FP instruction that returns within 14 cycles.
   - *Workaround:* every ISR has more than 2 instructions besides the return,
     or clear `FPCCR.LSPEN`.
   - *TIM:* only a concern for hand-written minimal ISRs.
9. **ES §2.1.3 Store-immediate overlapping exception return may vector to
   the wrong interrupt (A, Arm 838869), p.6-7.**
   - *Workaround:* set `ACTLR.DISDEFWBUF` (no MPU) or put a DSB before
     exception return.
   - *TIM:* consequence is that one handler runs twice and a level interrupt
     re-pends; rare. Decide project-wide.

### 7.2 FDCAN (both apply to all revisions)
- **ES §2.19.1 Desynchronisation with edge filtering (A), p.30.** If
  `CCCR.EFBI = 1` and the end of integration coincides with a falling edge on
  RX, the first bit is mis-received, the CRC fails and an error frame
  follows. *Workaround:* keep edge filtering off.
  - *TIM:* the HAL only sets EFBI in `HAL_FDCAN_EnableEdgeFiltering()`
    (`stm32g4xx_hal_fdcan.c` L1913), which TIM does not call, and the reset
    value is 0 (RM p.1987). **Not triggered; do not enable it.**
- **ES §2.19.2 Tx FIFO messages inverted (A), p.30-31.** Two consecutive Tx
  FIFO frames can go out swapped when FDCAN uses **both a dedicated Tx buffer
  and a Tx FIFO** and the dedicated buffer holds higher-priority frames.
  - *Workarounds:* only one FIFO element pending at a time; use only the
    FIFO; or ping-pong two dedicated buffers.
  - *TIM:* the G4 FDCAN has **no dedicated Tx buffers**; its 3 buffers are
    all FIFO/queue (RM §44.3.6 p.1973-1974). TIM uses
    `FDCAN_TX_FIFO_OPERATION`. The trigger condition cannot be built on this
    part as far as RM0440 shows. The erratum's workaround refers to "Tx
    buffer 4 and 5", which the G4 does not have, so the text appears to be
    inherited from the generic M_CAN.
- No FDCAN erratum touches bus-off, error counters, RX FIFOs or filters.

### 7.3 Flash and system (beyond §7.1)
- **ES §2.2.1 Full JTAG without NJTRST (A), p.7.** 4-wire JTAG fails if PB4
  is used for anything else. TIM uses PB4 as IMU_INT and debugs over SWD, so
  this is **not affected**. Do not switch to 4-wire JTAG.
- **ES §2.2.4 Unstable LSI when it clocks the RTC or CSS on LSE (P), p.8-9.**
  - *Condition:* LSI clocking the RTC or the CSS on LSE, and VDD reset
    without a backup-domain reset; with VBAT tied to VDD this includes a
    VDD drop under 1 ms.
  - *Effect:* LSI duty cycle off 50 % and frequency well above 32 kHz.
  - *Workaround:* do not clock the RTC from LSI (or use the CSS on LSE), or
    reset the backup domain at each power-up.
  - *TIM:* the IWDG also runs on LSI. **Do not put the RTC on LSI**, or IWDG
    timing (§5) could be affected. That IWDG link is my inference; the
    erratum names only RTC and CSS.
- **ES §2.2.7 Wrong instruction fetches after Sleep/Stop wake-up with debug
  in low-power mode (A), p.10.** Only with `DBG_SLEEP`/`DBG_STOP` set.
  *Workaround:* ISB after WFI/WFE. Debug builds only.
- **ES §2.2.8 PC13 transitions disturb the LSE (N), p.10.** Applies to LQFP
  packages, so TIM. Do not toggle PC13 if the LSE is used.
- ES §2.2.9 (Standby with HSE bypass) does not apply: TIM uses an 8 MHz
  crystal, not bypass.
- **No erratum on flash erase stall, on option bytes, or on the dual-bank
  boot.**

### 7.4 ADC (ADC1/2 used, with DMA)
- **ES §2.7.6 ADC slave data may be shifted (A), p.16.** Dual regular
  simultaneous mode, one DMA channel, OVRMOD = 0. *Workaround:* OVRMOD = 1,
  or one DMA channel per ADC. TIM is independent mode today; this matters if
  dual mode is adopted.
- **ES §2.7.10 ADC channel 0 converted instead of the requested channel (A,
  Rev Y only), p.17.** It returns 0 in four cases:
  1. an injected conversion while a regular one is in progress;
  2. the first conversions after a dual-mode resume;
  3. the first conversion after ADSTP/JADSTP;
  4. the first conversion after a DMA end of transfer in one-shot DMA mode.

  *Workaround:* a dummy conversion, or ADDIS/ADEN after a stop. Case 4 is
  exactly "ADC + DMA one-shot": **relevant if the board is Rev Y**.
- **ES §2.7.7 End of a 10/8/6-bit conversion disturbs other ADCs (A, Rev Z
  only), p.16.** Use 12-bit everywhere, which TIM does.
- **ES §2.7.8 Input channel switching disturbs ongoing conversions (P, Rev Z
  only), p.16-17.** Up to a few LSB DNL with concurrent ADCs. *Workaround:*
  bulb sampling for single-channel use, or convert each input twice and keep
  the first.
- Injected-mode items, relevant only if injected conversions are used:
  - §2.7.1 context launched without trigger (JQDIS = 0, JQM = 0), p.14-15;
  - §2.7.2 two contexts merged, p.15;
  - §2.7.3 spurious regular conversion in dual interleaved mode, p.15;
  - §2.7.5 injected data in the wrong JDRx after JADSTP (check JEOS first),
    p.16.
- **ES §2.7.4 `ADC_AWDy_OUT` reset by non-guarded channels (A), p.15-16.** Use
  only the rising edge, or guard every converted channel.
- ES §2.8.1 VREFBUF disturbed by PB10, PB11, PB13, PB14 toggling (P), p.18.
  **PB13/PB14 are SPI2 SCK/MISO on TIM.** This matters only if VREFBUF is
  enabled; it is not configured in `tim.ioc`.

### 7.5 DMA and DMAMUX
- **ES §2.3.1 DMA disable failure and error flag omission (A), p.11.** If
  software clears GIFx (CGIFx) in the same cycle as a transfer error, the
  channel is not auto-disabled and TEIFx is not set. *Workaround:* never clear
  GIFx while a channel is active; use the HTIF/TCIF/TEIF clears (the HAL
  already does this).
- ES §2.4.1-2.4.4 DMAMUX (N, N, N, A), p.11-12: only in DMAMUX
  synchronisation or request-generator modes. Not used by a plain ADC/SPI DMA
  setup.

### 7.6 Timers (TIM input capture)
- **No input-capture erratum.** The TIM errata are about one-pulse
  master/slave with MSM (§2.12.1, P), consecutive compare events in toggle
  mode (§2.12.2, N), center-aligned + dithering (§2.12.3, N), OCREF clear
  with external reset (§2.12.4, P), and bidirectional break with short pulses
  (§2.12.5, N), p.21-22. None affects capture.

### 7.7 I2C (I2C1 on PB8) and USART (USART1)
- **ES §2.15.1 Wrong SDA sampling when tSU;DAT is under one I2CCLK period
  (P), p.25-26.** I2CCLK must be at least 4 / 10 / 20 MHz for Standard /
  Fast / Fast-mode Plus.
- ES §2.15.2 spurious BERR in controller mode: clear it and carry on (A),
  p.26.
- **ES §2.15.5 transmission stalled after the first byte (A), p.27.** Happens
  when APB/I2CCLK is between 1.5 and 3 and TXDR is written within 2 kernel
  clocks. Pre-load TXDR or keep the ratio outside 1.5-3.
- ES §2.15.3, §2.15.4, §2.15.6: target-mode only, p.26-27.
- **ES §2.16.2 USART data corrupted by a glitch in the second half of the
  stop bit (A), p.28.** Workaround: a clean or filtered RX line.
- ES §2.16.1 (SPI-slave TC early), §2.16.3 (do not clear ABREN), §2.16.4 (NE
  with ONEBIT, N), p.28.

### 7.8 Not relevant to TIM
FMC (§2.5), QUADSPI (§2.6), COMP (§2.9.1, Rev Z/Y), OPAMP (§2.10.1), HRTIM
(§2.11), LPTIM (§2.13), RTC/TAMP (§2.14, unless the RTC is used), LPUART
(§2.17), USB (§2.20), UCPD (§2.21), and the core LDR-SP item §2.1.1 (only
with hand-written `LDR SP` assembly).

---

## Open items to close with the missing documents

Closed from RM0440 Rev 9 in the second pass:

1. ~~RM0440: GPIO reset values per port.~~ Done, §1.
2. ~~RM0440: the boot truth table and option-byte factory defaults.~~ Done,
   §2.
3. ~~RM0440: flash stall behaviour during erase and the 4 KB single-bank
   page size.~~ Done, §3.
4. ~~RM0440: FDCAN bus-off entry and recovery, and FIFO overwrite mode.~~
   Done, §4.
5. ~~RM0440: IWDG window semantics.~~ Done, §5.
6. ~~RM0440: SRAM1/SRAM2 contents across reset.~~ Done by omission, §6
   (no reset erase defined; confirm on a board).

Still open:

7. Backup domain and PC13-PC15 with VBAT **floating**: RM0440 only
   recommends tying VBAT to VDD with 100 nF; floating behaviour is not
   documented anywhere read. Fix in hardware rather than research further.
8. PINRSTF set or not on internal resets: not stated in RM0440; measure on a
   board.
9. Single-bank (4 KB) page erase time: not in DS12288 Table 47.
10. ~~AN2606: G474 bootloader interfaces, pins, pin states, and empty-flash
    check.~~ Done, §1 (third pass).
11. ~~ES0430: the complete errata list.~~ Done, §7 (third pass).

Still open after the third pass (none of these is answered by ES0430 or
AN2606):

12. **Silicon revision of the TIM boards:** read `DBGMCU_IDCODE.REV_ID`
    (0x2001 Z / 0x2002 Y / 0x2003 X). Rev Y adds ES §2.7.10; Rev Z adds
    §2.7.7 and §2.7.8.
13. **Bootloader version on the boards:** read 0x1FFF 6FFE (expect 0xD5).
14. Whether the bootloader sets `PWR_CR3.UCPD1_DBDIS`, i.e. whether the PB6
    / FDCAN2 dominant burst in §1 really happens: bench test (scope CAN2
    while forcing a bootloader entry).
15. Whether CAN traffic on PA11 (USB DM) can trigger the bootloader's DFU
    "cable detected" path: not described in AN2606.
16. ADM3057E STBY pull-down strength against the MCU pull-up on PA8 during
    bootloader I2C2 operation: the value is not given (ADM p.15).
17. Item 7 (VBAT floating), item 8 (PINRSTF) and item 9 (4 KB erase time)
    are unchanged. ES0430 adds §2.2.5 (missed backup-domain POR), which makes
    item 7 worse.
