# STM32G474 reset, flash, FDCAN and watchdog facts

Answers issue #6. Part TIM rev A: STM32G474RET3, LQFP64, 512 KB flash.

## Read this first: what was and was not read

| Source | Revision | Read? |
|---|---|---|
| DS12288, STM32G474xB/xC/xE datasheet (`Datasheets/stm32g474cb.pdf`) | Rev 6, November 2021 (footer, p.1) | Yes |
| ST CMSIS device header `Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h` | CMSIS STM32G4 V1.2.6 (`stm32g4xx.h` L93-96) | Yes |
| ST HAL/LL headers and sources in `Drivers/STM32G4xx_HAL_Driver/` | HAL V1.2.7 (`stm32g4xx_hal.c` L53-55) | Yes |
| RM0440, STM32G4 reference manual | none | **No** |
| ES0430, STM32G471/473/474/483/484 errata sheet | none | **No** |
| AN2606, STM32 system memory boot mode | none | **No** |

st.com did not respond from this machine: curl, PowerShell and WebFetch all
timed out with 0 bytes received, while github.com answered at once. The site
appears to block automated clients, and that block was not bypassed. No local
copies were found under Downloads, Documents or `~/Git`.

So the fallback was ST's own CMSIS/HAL source, which ships with the project.
It is first-party ST material and authoritative for **register bit positions
and option-byte encodings**. Its comments are secondary for **behaviour**, so
behaviour facts taken only from a HAL comment are marked as such.

Anything that needs RM0440, ES0430 or AN2606 is marked **UNVERIFIED**. That
covers the whole errata section and the bootloader pin states. Re-run this
ticket's unverified items once those PDFs are in `Datasheets/`.

Citation forms used below:
- `DS p.N` is DS12288 Rev 6, printed page N of 236.
- `CMSIS Lnnnn` is a line in `stm32g474xx.h`.
- `HAL <file> Lnnn` is a line in the named HAL file.

"Category 3" (the 512 KB G47x/G48x class) is RM0440's term. DS12288 does not
use it, so it is not confirmed here. Every flash fact below is for the
512 KB part.

---

## 1. Pin states through reset and the bootloader

### General I/O
- **During and after reset**, DS12288 gives two descriptions that do not agree:
  - §3.11.5 "Reset mode": the I/Os are in **"analog state"** (Schmitt trigger
    disabled) both under and after reset (DS p.25).
  - Table 11: "Unless otherwise specified by a note, all I/Os are set as
    **floating inputs** during and after reset" (DS p.56).

  Either way, no pin drives and none has a pull, apart from the exceptions
  below. The exact per-port `GPIOx_MODER` reset values are in RM0440 and are
  **UNVERIFIED** here.
- **NRST**: the internal reset pull-up is deactivated when the reset source is
  internal (DS p.25). Its weak pull-up R_PU is 25/40/55 kΩ min/typ/max, and the
  input filter passes pulses of 70 ns max (Table 58, DS p.133). The NRST pin
  mode is the option field `NRST_MODE[1:0]`: input-only, GPIO, or
  input+output (CMSIS L5288; HAL `stm32g4xx_hal_flash.h` L442-444).
- **Internal pull-ups and pull-downs** are 25/40/55 kΩ min/typ/max
  (Table 54, DS p.128).

### SWD and JTAG pins (LQFP64: PA13 = pin 49, PA14 = pin 50, PA15, PB3, PB4)
- After reset these pins are already configured as the JTAG/SW debug
  alternate functions. PA15, PA13 and PB4 have their **internal pull-up on**,
  and PA14 has its **internal pull-down on** (Table 12 note 5, DS p.72).
  SWD therefore works out of reset without any firmware.

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

### During the system bootloader
- **UNVERIFIED (needs AN2606).** The only DS statement is that the bootloader
  can reprogram flash over USART, I2C, SPI and USB DFU (§3.7, DS p.21).
  AN2606 gives the exact pins the G474 bootloader drives or samples (including
  any I2C1 and FDCAN pins), plus its own GPIO and clock settings. Not
  confirmed here.

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
- **Unverified (RM0440 FLASH chapter):**
  - factory default values of `nBOOT1`, `nBOOT0`, `nSWBOOT0`, `DBANK`,
    `BOR_LEV` and the IWDG bits;
  - the full boot-mode truth table, including empty-flash detection;
  - option-byte loading semantics (OBL_LAUNCH).

  The CLAUDE.md note says the factory `nSWBOOT0` is 1; that is a project
  observation and was not checked against the RM.
- **Debug freeze is not an option byte.** IWDG and WWDG freeze under debug is
  `DBGMCU_APB1FZR1.DBG_IWDG_STOP` (bit 12) and `DBG_WWDG_STOP` (bit 11)
  (CMSIS L3165-3168).

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
  the bank (CMSIS L5166-5168). The 4 KB single-bank page size comes only from
  the HAL header; it is **not confirmed in RM0440 here**.
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
- **Does an erase stall execution?** The DS shows only the positive half: RWW
  allows "a read operation from one bank while an erase or program operation
  is performed to the other bank", in dual bank mode (§3.4, DS p.19).
  - **Implied but UNVERIFIED (RM0440):** reading the bank being
    erased/programmed, or any read in single bank mode, stalls the bus until
    the operation completes. A 2 KB page erase would then stall code fetched
    from that bank for up to about 24.5 ms. To erase without stalling, run
    dual bank mode with code in bank 1 and data pages in bank 2, or execute
    the critical path from RAM.
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
- **There are no dedicated RX buffers.** The RX path is only the two 3-deep
  FIFOs, so at high bus load firmware must drain them quickly (ISR or DMA-free
  copy). FIFO overflow behaviour, blocking versus overwrite (`RXGFC.F0OM` and
  `F1OM`), is RM0440 material and **UNVERIFIED** here.
- **TX modes.** FIFO mode (`TXBC.TFQM = 0`) or Queue mode (`TFQM = 1`, lowest
  ID sent first) (HAL `stm32g4xx_hal_fdcan.h` L583-584; CMSIS L4985).
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
- **Bus-off.** Entry is signalled by `PSR.BO` and the `IR.BO` interrupt.
  - **UNVERIFIED (needs RM0440):** the entry condition (TEC > 255), whether
    hardware sets `CCCR.INIT` on bus-off, and the recovery sequence (software
    clears `INIT`, then the controller waits for 128 occurrences of 11
    recessive bits). These match the Bosch M_CAN design the G4 FDCAN derives
    from, but they were not read in an ST document here.
  - Also relevant: `CCCR.DAR` (bit 6, disable automatic retransmission;
    CMSIS L4574) and Restricted Operation mode (HAL `stm32g4xx_hal_fdcan.c`
    L2596-2625).

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
- **Window mode.** The `IWDG_WINR` register exists, with `WIN[11:0]` at bit 0
  (CMSIS L10636). Its semantics (refresh only allowed below WIN) are
  **UNVERIFIED**.
- **Stop and Standby.** Freezing is controlled by option bits `IWDG_STOP` and
  `IWDG_STDBY`, where 0 freezes and 1 runs (§2 above).
- **Debug.** Freezing is controlled by `DBGMCU_APB1FZR1.DBG_IWDG_STOP`
  (bit 12, CMSIS L3168). It must be set by firmware or the debugger to stop
  the IWDG resetting the part at a breakpoint.
- **Hardware-start mode.** `IWDG_SW = 0` starts the IWDG automatically at
  reset (HAL L332). Once started, the IWDG cannot be stopped: generic STM32
  behaviour, **UNVERIFIED** in RM0440 here.

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
- **SRAM layout** (§3.5, DS p.20; CMSIS L1134-1152):

  | Region | Size | Address | Notes |
  |---|---|---|---|
  | SRAM1 | 80 KB | 0x2000_0000 | First 32 KB has parity |
  | SRAM2 | 16 KB | 0x2001_4000 | Can be retained in Standby (`PWR_CR3.RRS`, CMSIS L10820) |
  | CCM SRAM | 32 KB | 0x1000_0000, aliased at 0x2001_8000 | Has parity; write-protect in 1 KB granules |

- **What survives a system reset (NRST, software, IWDG):**
  - **CCM SRAM is erased on system reset by default** (`CCMSRAM_RST = 0`;
    HAL L416-417). Clear that bit's effect by setting the option to 1 if CCM
    is to hold a crash record.
  - SRAM1 and SRAM2 contents across a system reset: the DS does not say. They
    are not hardware-erased on other STM32 families, but that is
    **UNVERIFIED** for G474 (RM0440). With parity enabled, reading
    never-written SRAM may raise a parity error; also UNVERIFIED.
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
  - Writing them needs backup-domain write access (`PWR_CR1.DBP`). That is
    RM0440 material, **UNVERIFIED** here.
  - Practical read: the backup registers are a fair place for a
    reset-reason or crash record across a watchdog reset. Do not rely on them
    across power cycles.

---

## 7. Errata (ES0430): ALL UNVERIFIED

ES0430 could not be fetched, so **no erratum is confirmed here**. The ticket
asks for every erratum touching FDCAN, flash, ADC, SPI and the watchdog. That
list must be built from ES0430 itself (latest revision, matched to the
silicon revision read from `DBGMCU_IDCODE.REV_ID` on a TIM board).

Look in particular at the FDCAN, FLASH, ADC, SPI/I2S and IWDG/WWDG sections,
and the "System" section (debug, boot and option-byte items). Do not take any
erratum from memory into a design decision without the sheet.

---

## Open items to close with the missing documents

1. RM0440: GPIO reset values per port.
2. RM0440: the boot truth table and option-byte factory defaults.
3. RM0440: flash stall behaviour during erase and the 4 KB single-bank page
   size.
4. RM0440: FDCAN bus-off entry and recovery, and FIFO overwrite mode.
5. RM0440: IWDG window semantics.
6. RM0440: SRAM1/SRAM2 contents across reset.
7. RM0440: backup domain with VBAT floating, and PINRSTF on internal resets.
8. AN2606: G474 bootloader interfaces, pins and pin states.
9. ES0430: the complete errata list for FDCAN, flash, ADC, SPI and watchdog.
