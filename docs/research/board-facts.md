# Board facts from the schematics

Answers issue #7. Every fact cites its source. Items marked **UNVERIFIED** could not
be read with confidence or confirmed against a primary source; do not build on them
without checking the PCB source or the board.

## Sources

| Tag | Document | Revision read |
|---|---|---|
| TIM | `Datasheets/tim-1.pdf`, TIM schematic (Altium), 9 sheets | Rev 1 ("rev A"), issued 10/13/2026 |
| MST | `Datasheets/bms_master-schematic.pdf`, BMS master daughterboard (KiCad), 10 sheets | "Unreleased / PRELIMINARY", cover 2026-11-04, sheets 2026-10-20 |
| SLV | `Datasheets/bms_slave-schematic.pdf`, BMS slave (KiCad) | Rev 1.2.0 "RELEASED", cover 2026-11-03 |
| ADM | `Datasheets/adm3055e-adm3057e.pdf`, Analog Devices ADM3055E/ADM3057E | Rev D |
| G4 | `Datasheets/stm32g474cb.pdf`, ST DS12288 | Rev 6 |
| B600 | `Datasheets/bq79600-q1.pdf`, TI SLUSDS1A | Aug 2020 |
| IOC | `tim.ioc` and `Core/Src/main.c` in this repo (the firmware's current view) | commit 18cccc3 |

Citation form: `TIM s4 U1` means TIM schematic, sheet 4, designator U1. Datasheet
citations give the page number.

Method note: the schematics are drawings, so they were read visually. A small number
of nets were too small to resolve and are marked UNVERIFIED in place. AN2606 (the ST
bootloader pin usage) is not in `Datasheets/`, so the pin states *while in the
bootloader* are only partly confirmed.

---

## 1. TIM MCU pin table

MCU is U1 on TIM s4. The schematic symbol and the block diagram (TIM s2) say
**STM32G474RCT6**; `tim.ioc` (`Mcu.UserName=STM32G474RETx`) and CLAUDE.md say RET3.
See rev B finding R1.

### Generic reset behaviour (applies to every row unless the row says otherwise)

- All I/Os are floating inputs during and after reset (G4 p.56, Table 11 notes).
- After reset PA13 (SWDIO) and PA15 and PB4 have the internal pull-up enabled, PA14
  (SWCLK) the internal pull-down, because they come up as JTAG/SWD (G4 p.72 note 5).
- UCPD dead-battery: after reset a 5.1 kohm pull-down is active on **PB6** while **PA9**
  is high, and on **PB4** while **PA10** is high, until firmware sets
  `PWR_CR3.UCPD1_DBDIS` (G4 p.72 note 6). Firmware does this in
  `HAL_MspInit` (`Core/Src/stm32g4xx_hal_msp.c:78`,
  `HAL_PWREx_DisableUCPDDeadBattery()`), so this applies only before firmware runs, in
  the bootloader, and on a blank part.
- "Unprogrammed" and "in reset" are the same pin state (floating, plus the pulls above).
  Bootloader: the ST system bootloader may claim USART, I2C, SPI, FDCAN and USB pins
  while it scans for a host. **UNVERIFIED**: AN2606 is not in the folder.
- Boot source: PB8-BOOT0 has 2 kohm to 3V3 (R2 or R3 via R1 0R, TIM s4), so with the
  factory option bytes the part boots the **bootloader**, not flash (TIM s4 design
  note; G4 p.21 s3.7; CLAUDE.md "First-time board setup").

### Pin table

"Reset state" means the electrical state of the net with the MCU in reset, unprogrammed
or held by the bootloader (subject to the bootloader caveat above).

| Pin | Port | Net (TIM s4) | External pull / load | Goes to | Reset / unprogrammed state | `tim.ioc` |
|---|---|---|---|---|---|---|
| 1 | VBAT | VBAT | Drawn with a no-connect cross (**UNVERIFIED**, small) | none | n/a | n/a. See R2 |
| 2 | PC13 | CAN_TERM_2 → CAN_TERM_TOGGLE_2 | none drawn | U2 (CAN2) AUXIN, TIM s5 | **Floating**, so CAN2 termination is undefined, see section 2 | `Can_Term_2` output, init LOW |
| 3 | PC14-OSC32_IN | GPIO_7 | none on TIM | J3-20 (mezz) | Floating | `GPIO_7` output, init LOW. **Conflicts on BMS**, R6 |
| 4 | PC15-OSC32_OUT | GPIO_GNDDETECT | none on TIM; the ST-LINK V3MINI grounds it when attached (TIM s7 note) | J2-11 (STDC14) | Floating | input, pull-up. Matches |
| 5, 6 | PF0, PF1 | XIN, XOUT | X1 8 MHz, C11 C12 12 pF | crystal | HSE off | HSE. Matches |
| 7 | PG10-NRST | NRST | C10 100 nF to GND; internal weak pull-up | J2-12 | n/a | n/a |
| 8 | PC0 | TIM1_CH1 | none | J3-9 | Floating | TIM1_CH1. Matches |
| 9 | PC1 | TIM1_CH2 | none | J3-10 | Floating | Matches |
| 10 | PC2 | TIM1_CH3 | none | J3-11 | Floating | Matches |
| 11 | PC3 | VBAT_ADC | R21 120k / R22 10k divider from VBAT (before fuse), D22 ESD, TIM s8 | battery divider | Analog input; divider always loaded | ADC1_IN9. Matches |
| 12 | PA0 | ADC1_IN1 | none on TIM (no RC filter drawn) | J5-1,2 | Floating | ADC1_IN1. Matches |
| 13 | PA1 | ADC1_IN2 | none | J5-5,6 | Floating | Matches |
| 14 | PA2 | ADC1_IN3 | none | J5-9,10 | Floating | Matches |
| 17 | PA3 | ADC1_IN4 | none | J5-13,14 | Floating | Matches |
| 18 | PA4 | DAC1_OUT1 | none (s4 note: DAC drives only 50 pF, series R?) | J5-25,26 | Floating | COMP_DAC11_group. Matches |
| 19 | PA5 | DAC1_OUT2 | none | J5-21,22 | Floating | Matches |
| 20 | PA6 | GPIO_1 | none on TIM | J3-14 | Floating | `GPIO_1` output, **init LOW**. See R7 |
| 21 | PA7 | GPIO_2 | none on TIM | J3-15 | Floating | `SharedStack_PA7` |
| 22 | PC4 | ADC2_IN5 | none | J5-17,18 | Floating | ADC2_IN5. Matches |
| 23 | PC5 | GPIO_3 | none on TIM | J3-16 | Floating | `SharedStack_PC5` |
| 24 | PB0 | GPIO_4 | none on TIM | J3-17 | Floating | `SharedStack_PB0` |
| 25 | PB1 | GPIO_5 | none on TIM | J3-18 | Floating | `SharedStack_PB1` |
| 26 | PB2 | GPIO_6 | none on TIM | J3-19 | Floating | `SharedStack_PB2` |
| 28 / 29 | VREF+ / VDDA | 3V3_FILT via FB1 | C6 100n, C7 1u / C8 1u, C9 10n | | | |
| 30 | PB10 | TIM2_CH3 | none | J3-13 | Floating | Matches |
| 33 | PB11 | SD_DETECT | R11..R16 bank 10k to 3V3 (one of them), D18 ESD; Q2 gate (card LED D6) | J1-10 microSD | Pulled high (no card) | input, pull-up. Matches |
| 34 | PB12 | SPI2_CS (SD_CS) | 10k pull-up (R11..R16 bank) | J1-2 | High (card deselected) | output init HIGH. Matches |
| 35 | PB13 | SPI2_SCK | 10k pull-up (bank) | J1-5 | High | Matches |
| 36 | PB14 | SPI2_MISO | 10k pull-up (bank) | J1-7 | High | Matches |
| 37 | PB15 | SPI2_MOSI | 10k pull-up (bank) | J1-3 | High | Matches |
| 38 | PC6 | GPIO_CAN2LED | D3 + R6 240R from 3V3 (active low) | LED | LED **off** (pin floating) | output init HIGH (off). Matches |
| 39 | PC7 | GPIO_CAN1LED | D2 + R5 240R from 3V3 | LED | LED off | Matches |
| 40 | PC8 | GPIO_HBLED | D1 + R4 240R from 3V3 | LED | LED off | Matches |
| 41 | PC9 | CAN_TERM_1 → CAN_TERM_TOGGLE_1 | none drawn | U2 (CAN1) AUXIN, TIM s5 | **Floating**, see section 2 | `Can_Term_1` output, init LOW |
| 42 | PA8 | FDCAN1_STB | none drawn (ADM internal pull-down) | U2 (CAN1) STBY | Floating → transceiver **normal mode** (ADM p.15) | output init HIGH (standby) |
| 43 | PA9 | USART1_TX → T_VCP_TX | none | J2-14 | Floating; a high level here enables the PB6 dead-battery pull-down | USART1_TX. Matches |
| 44 | PA10 | USART1_RX → T_VCP_RX | none | J2-13 | Floating, or driven high by an attached ST-LINK (UART idle); enables the PB4 dead-battery pull-down | Matches |
| 45 | PA11 | FDCAN1_RX | none | U2 (CAN1) RXD | Input, driven by transceiver | Matches |
| 46 | PA12 | FDCAN1_TX | none (ADM weak pull-up to VIO) | U2 (CAN1) TXD | Floating → recessive via ADM pull-up (ADM p.15) | Matches |
| 49 | PA13 | SWDIO | internal pull-up | J2-4 | SWD | Matches |
| 50 | PA14 | SWCLK | internal pull-down | J2-6 | SWD | Matches |
| 51 | PA15 | TIM2_CH1 | **internal pull-up after reset** (G4 p.72 n5) | J3-12 | **Pulled high** | Matches |
| 52 | PC10 | SPI3_SCK (IMU_SCK) | 10k pull-up, one of R24..R26 (**UNVERIFIED** which three of the four IMU lines carry the pull-ups); R27 0R to SPI_SCK | U3 SCL; J3-26 | High (if pulled) | Matches |
| 53 | PC11 | SPI3_MISO (IMU_MISO) | see above; R29 0R to SPI_MISO | U3 SDO/SA0; J3-22 | see R3 | Matches |
| 54 | PC12 | SPI3_MOSI (IMU_MOSI) | see above; R28 0R to SPI_MOSI | U3 SDA; J3-21 | see above | Matches |
| 55 | PD2 | SPI3_CS (IMU_CS) | see above | U3 CS | High if pulled | `SPI3_CS` output init HIGH. Matches |
| 56 | PB3 | SWO | none | J2-8 | SWO | Matches |
| 57 | PB4 | IMU_INT | none; **internal pull-up after reset**, plus 5.1k dead-battery pull-down while PA10 high | U3 INT1 | Driven by IMU | EXTI rising, no pull. Matches |
| 58 | PB5 | FDCAN2_RX | none | U2 (CAN2) RXD | Driven | Matches |
| 59 | PB6 | FDCAN2_TX | ADM weak pull-up; **5.1k dead-battery pull-down while PA9 high** | U2 (CAN2) TXD | Normally recessive; see R5 | Matches |
| 60 | PB7 | I2C1_SDA | R2 or R3 2k to 3V3 | J3-30 | High | Matches |
| 61 | PB8-BOOT0 | I2C1_SCL | R1 0R in series, 2k to 3V3 | J3-29 | High, so **BOOT0 = 1** | Matches |
| 62 | PB9 | FDCAN2_STB | none (ADM internal pull-down) | U2 (CAN2) STBY | Floating → **normal mode** | output init HIGH |
| 16, 32, 48, 64 | VDD | 3V3 | C1 4.7u, C2..C5 100n | | | |

Name cross-check against `tim.ioc`: every net above lands on the same port in
`tim.ioc`. The only label differences are cosmetic (`Can_Term_1/2` vs `CAN_TERM_1/2`,
`SPI3_CS` vs `IMU_CS`). The MCU part number (R1) and the GPIO_x directions on the BMS
(R6, R7) are the real mismatches.

## 2. CAN transceivers and termination before firmware runs

Both buses use the same circuit (TIM s5, two instances, U2 = ADM3057EBRWZ).

- **Wiring** (TIM s5): VCC = 5V, VIO = 3V3, SILENT pin 6 unconnected, RS pin 12 to
  ISO_GND via R7 0R (full-speed mode, ADM p.15), STBY ← FDCANx_STB, AUXIN ← CAN_TERM_TOGGLE_x,
  AUXOUT → TERM_TOGGLE. Bus side has its own isolated 5V (VISOOUT to VISOIN via FB4)
  and ISO_GND, so each bus is galvanically isolated from the TIM logic ground.
- **STBY undriven**: STBY has an internal pull-down; low or open means **normal
  mode** (ADM p.15 pin 8). So in reset, unprogrammed or the bootloader, both
  transceivers are **active on the bus**. Standby is a firmware choice
  (`MX_GPIO_Init` drives STB high), not a hardware default. The comment in
  `App/Src/can_if.c:160` ("high = standby (default at boot)") is correct only once
  `MX_GPIO_Init` has run.
- **TXD undriven**: TXD has a weak internal pull-up to VIO (ADM p.15 pin 7), so a
  floating FDCAN_TX gives a recessive bus. The exception is CAN2 (R5).
- **SILENT** unconnected: internal pull-down, normal mode (ADM p.15 pin 6).
- **Termination**: AUXOUT drives a TLP172AM photorelay LED (D5) through R9 240R to
  ISO_GND. The relay contact is normally open (TIM s5 note "NO"). The contact closes
  the CANH-to-CANL termination, R10 59R + R23 59R (about 118 ohm in series), with C16 C17
  68 pF to ISO_GND. The exact split-termination topology is as read and
  **UNVERIFIED**.
- **Termination before firmware**: "AUXOUT is low at startup or when VIO is
  unpowered", and "the state of AUXOUT is latched when STBY is high" (ADM p.15 pin 17;
  p.24). So at power-up the **termination is open**. After that, with STBY floating
  low (normal mode), AUXOUT follows AUXIN, and AUXIN is a **floating MCU pin** (PC9,
  PC13) with no external pull drawn. The datasheet gives no internal pull for AUXIN
  (ADM p.15 pin 9 says only "sets the AUXOUT output"). So **termination is undefined
  while the MCU is in reset or unprogrammed**. See R4.
- **Termination once firmware runs**: `MX_GPIO_Init` drives Can_Term_1/2 low (open)
  and STB high (standby, which latches AUXOUT). `can_if.c` then drops STB low and
  leaves termination to `can_set_termination()`.
- The bus connectors are J4 (CAN1) and J6 (CAN2), Molex 0436500300: pin 1 CANFD_P,
  2 CANFD_N, 3 ISO_GND_x. D10 and D11 are PESD2CANFD24LT-QR (TIM s7).

## 3. Mezzanine connectors (TIM s7)

**J3, digital, Samtec SFM-115-02-L-D-A-K-TR (2x15)**

| Pin | Net | MCU | Pin | Net | MCU |
|---|---|---|---|---|---|
| 1, 2 | 12V | | 3, 4 | 5V | |
| 5, 6 | 3V3 | | 7, 8 | GND | |
| 9 | TIM1_CH1 | PC0 | 10 | TIM1_CH2 | PC1 |
| 11 | TIM1_CH3 | PC2 | 12 | TIM2_CH1 | PA15 (pulled up in reset) |
| 13 | TIM2_CH3 | PB10 | 14 | GPIO_1 | PA6 |
| 15 | GPIO_2 | PA7 | 16 | GPIO_3 | PC5 |
| 17 | GPIO_4 | PB0 | 18 | GPIO_5 | PB1 |
| 19 | GPIO_6 | PB2 | 20 | GPIO_7 | PC14 |
| 21 | SPI_MOSI | PC12 via R28 0R | 22 | SPI_MISO | PC11 via R29 0R |
| 23, 24, 25 | GND | | 26 | SPI_SCK | PC10 via R27 0R |
| 27, 28 | GND | | 29 | I2C1_SCL | PB8 (2k pull-up) |
| 30 | I2C1_SDA | PB7 (2k pull-up) | | | |

There is no SPI chip select on the mezzanine: daughterboards must use a GPIO_x pin
as CS. The SPI lines are SPI3, shared with the onboard IMU (TIM s6 R27..R29).

**J5, analog, 2x15**: 1,2 ADC1_IN1 (PA0); 5,6 ADC1_IN2 (PA1); 9,10 ADC1_IN3 (PA2);
13,14 ADC1_IN4 (PA3); 17,18 ADC2_IN5 (PC4); 21,22 DAC1_OUT2 (PA5); 25,26 DAC1_OUT1
(PA4); all others GND (3,4, 7,8, 11,12, 15,16, 19,20, 23,24, 27..30).

ESD: D19..D43 on the mezzanine signals; D7 ESD9B3.3 on 3V3; D8 ESD9B5.0 on 5V. In the
"TVS" position on 12V, D9 is a CDBA340L-HF Schottky (TIM s7). See R8.

The block diagram (TIM s2) lists RC filters on the analog inputs and UART, QEI and
high-side-switch functions on the mezzanine. None of these is drawn on s7 (R9).

**J2, debug, Samtec FTSH-107 (STDC14)**: 3 3V3, 4 SWDIO, 5 GND, 6 SWCLK, 7 GND,
8 SWO, 11 GNDDETECT, 12 NRST, 13 T_VCP_RX, 14 T_VCP_TX; 1, 2, 9, 10 NC.

## 4. BMS master daughterboard

### 4.1 BMS_OK path (MST s7) and the missing watchdog

As drawn:

```
MCU_BMS_OK (J3-17, TIM GPIO_4 / PB0) --+-- R17 10k to GND
                                       +-- U4 SN74LVC1G08 A (pin 1)
NFAULT (BQ79600 pin 2, R2 100k to 3V3) --- U4 B (pin 2)
U4 Y (pin 4) = BMS_OK_LOGIC -- R15 330R -- BMS_OK -- J5-1 (to car), U5 ESD9B5.0 to GND
```

- **BMS_OK = MCU_BMS_OK AND NFAULT.** That is all the hardware does.
- **Safe level is LOW.** R17 holds MCU_BMS_OK low while the MCU is in reset,
  unprogrammed or in the bootloader (PB0 floats). The design note on MST s7 says the
  LVC outputs have Ioff and go high-impedance without 3V3, so "loss of 3V3 = off".
  R15 330R limits the drive into the external optocoupler (note: about 6 mA nominal,
  3 mA worst case).
- **There is no window watchdog on the schematic.** The MST s7 design note states
  `BMS_OK = NFAULT AND MCU_BMS_OK AND WDT_OK` and specifies a **TPS3430**: CWD = 10k,
  SET0 = 0, SET1 = 1, giving a window of 10.35 ms to 165.8 ms; CRST = 4.7 nF giving
  about 15.5 ms reset delay; and it says "U5 holds CRST low". But no TPS3430 is drawn.
  The sheet has only U4 (AND) and U5 (an ESD9B5.0 diode, not a supervisor). The
  WDT_KICK sheet port (MST s3) is not connected to anything on s7 and does not appear
  on J3. The window figures are the note's numbers only: **UNVERIFIED** (no TPS3430
  datasheet in the folder, and no components are placed to check them against). See R10.
- NFAULT is pulled up by R2 100k to 3V3 and is open drain (B600 p.3). If the BQ79600 is
  unpowered (12V absent) or its NFAULT output is high-impedance, NFAULT reads
  **high (no fault)**. So the AND does not detect a dead bridge (R11).
- Indicators (MST s7): D4 green lit when BMS_OK_LOGIC is high (R16 1k). U6
  SN74LVC2G04 inverts FB.BMS_OK and FB.NFAULT to light red D5 and D6 (R20, R21 1k)
  when the respective signal is low. D3 green is 3V3 present (R14 1k).
- Readback: R18 10k from BMS_OK_LOGIC to FB.BMS_OK, and R19 10k from NFAULT to
  FB.NFAULT, both on J3 so the MCU can read them.

### 4.2 Signals between the BMS master and the TIM

Assumes the TIM J3 socket (SFM) mates pin-for-pin with the master J3 header (TFM),
and TIM J5 with master J4. That matches the matching-numbered drawings, but
connector orientation is **UNVERIFIED** against the PCB.

| Master pin (MST s4) | Master net | Direction | TIM net | MCU pin | Notes |
|---|---|---|---|---|---|
| J3-1,2 / 3,4 / 5,6 | 12V / 5V / 3V3 | TIM → master | supplies | | master has no regulators |
| J3-9 | wire, **no readable label** | ? | TIM1_CH1 | PC0 | **UNVERIFIED**; possibly the intended WDT_KICK |
| J3-14 | SPI.nCS | out | GPIO_1 | PA6 | BQ79600 nCS (pin 7) |
| J3-15 | SPI.RDY | in | GPIO_2 | PA7 | BQ79600 nUART/SPI (SPI_RDY), pin 8 |
| J3-16 | FB.NFAULT | in | GPIO_3 | PC5 | NFAULT via R19 10k |
| J3-17 | MCU_BMS_OK | out | GPIO_4 | PB0 | AND input A, R17 10k pull-down |
| J3-18 | CHG_MODE_N | in | GPIO_5 | PB1 | low when J5-3 is grounded (charger) |
| J3-19 | FB.BMS_OK | in | GPIO_6 | PB2 | BMS_OK_LOGIC via R18 10k |
| J3-20 | SDC_OK_N | in | GPIO_7 | PC14 | low when the shutdown circuit is energised |
| J3-21 | SPI.MOSI | out | SPI_MOSI | PC12 (SPI3) | BQ79600 pin 4 |
| J3-22 | SPI.MISO | in | SPI_MISO | PC11 (SPI3) | BQ79600 pin 5. **Shared with IMU**, R3 |
| J3-26 | SPI.SCLK | out | SPI_SCK | PC10 (SPI3) | BQ79600 pin 6 |
| J3-7,8,23..25,27,28 | GND | | GND | | common ground with the TIM |
| J3-10..13, 29, 30 | not connected | | TIM1_CH2/3, TIM2_CH1/3, I2C1 | | |
| J4-1,2 | BOARD_TEMP | in | ADC1_IN1 | PA0 | TH1 NCP18XH103 10k NTC from 3V3, R26 10k to GND, C15 100n; voltage **rises with temperature** (MST s8) |
| J4 rest | GND or NC | | | | |

Input circuits (MST s8):
- **SDC_SENSE**: J5-6 → R23 10k → U7 TLP293 LED → J5-5 SDC_SENSE_RTN, D7 1N4148W
  antiparallel. The transistor's collector has R22 22k to 3V3 and C13 10 nF, so
  **SDC_OK_N is active low** (low means current flows in the shutdown loop).
- **CHG_MODE**: J5-3 with R24 10k pull-up to 3V3, U8 ESD, R25 10k / C14 10 nF filter.
  **CHG_MODE_N** is low when the external pin is grounded.
- External connector J5 (Molex 0436500600): 1 BMS_OK, 2 GND, 3 CHG_MODE, 4 GND,
  5 SDC_SENSE_RTN, 6 SDC_SENSE.

### 4.3 BQ79600 connections (MST s5, U1 BQ79600PWRQ1)

- SPI mode: nCS, SCLK, MOSI, MISO to J3 as above; nUART/SPI (SPI_RDY) to J3-15. VIO
  = 3V3 (C3 100n). The datasheet requires a pull-up on nCS, MOSI, MISO and SPI_RDY and
  a pull-down on SCLK (B600 p.3). Drawn: R3, R4 10k pull-ups; R5, R6 10k pull-ups
  marked DNP; R7 10k pull-down. Which resistor is on which net could not be resolved
  with confidence: **UNVERIFIED**. If R5 and R6 are the MOSI and MISO pull-ups, then
  MOSI relies on the TIM's R24..R26 (R12).
- NFAULT pin 2, R2 100k to 3V3.
- BAT pin 15 from 12V via R1 10R, C2 100n. **INH pin 16 tied to BAT**, so reverse
  wake is not used (B600 p.3: "If reverse wake up feature is not used, connect this
  pin to BAT").
- **Wake**: no dedicated wake line. The device wakes on a WAKE ping from the host on
  the comm interface (B600 p.7 tSU(WAKE_SHUT) 2 to 3.5 ms after the ping;
  tPOR2COMM 1 ms after BAT rises, p.6). The exact SPI-mode ping sequence is in B600
  s7.3.2.1.2.2.1 and was not transcribed here.
- DVDD pin 1 C1 220n; CVDD pin 14 C4 220n.
- **MISO is not tri-stated**: "MISO pin drives high in SPI idle mode, cannot directly
  support SPI multidrop ... add a tri-state buffer" (B600 p.3). See R3.

### 4.4 Isolation and supply

- Daisy chain (MST s6): two ports, COMH and COML, each through an SM91502ALA-E pulse
  transformer (U2, U3), with 49.9R series resistors (R8 R10, R11 R13), 1k
  termination (R9, R12), 100 pF line capacitors to GND, a 100 pF centre-tap capacitor
  and PESD5V0U2BT TVS (D1, D2). Both ports are wired, so a ring topology is possible.
  The board note specifies 220 pF capacitors and PESD1CAN TVS, but 100 pF and
  PESD5V0U2BT are drawn (R13). The note gives a 7.11 mm creepage pad gap and requires
  conformal coat.
- Connectors: J1 0430450200 and J2 0436500200. The s6 note swaps which is COMH and
  COML relative to the symbols (R13).
- **The transformers are the only isolation.** The master's GND is the TIM's GND
  (J3 GND pins). All rails (12V, 5V, 3V3) come from the TIM over J3; there is no
  regulator on the master. The BQ79600 BAT is the 12V rail.

## 5. BMS slave (SLV, U1 BQ79616PAPRQ1)

- **Configuration** (SLV s1, s4): 8S per slave (8 Enepaq 1S4P Molicel P50B modules),
  23.2 V to 33.6 V module; 16 slaves gives 128S. Rev 1.2.0 RELEASED.
- **Cell channels** (SLV s5): J3 (IPS1-110, 2x10) carries TAP0..TAP8. Cell n is
  between TAP(n-1) and TAPn and lands on VCn and CBn, n = 1..8. Each tap goes through
  R_VC 100R to VCn and R_CB 10R to CBn (pairs R2/R3 for TAP8 down to R18/R19 for TAP0).
  C_VC 0.47 uF differential between adjacent VC pins (C2, C5..C12); C_CB 0.47 uF between
  adjacent CB pins (C4, C13..C20); VC0 and CB0 caps to GND. **VC9..VC16 and CB9..CB16
  are tied to BAT** (unused upper channels).
- **J3 pinout**: 1 TAP8, 2 VMODULE, 3 TAP7, 4 TMP8, 5 TAP6, 6 TMP7, 7 TAP5, 8 TMP6,
  9 TAP4, 10 TMP5, 11 TAP3, 12 TMP4, 13 TAP2, 14 TMP3, 15 TAP1, 16 TMP2, 17 TAP0,
  18 TMP1, 19 GND, 20 GND.
- **Supply** (SLV s6): BAT from VMODULE via R1 30R, C1 10 nF; Q1 ZXTN4004K NPN pass
  for the internal LDO, collector from VMODULE via R20 + R21 150R.
- **Sense fusing**: **none on the slave.** The SLV s4 note puts the tap fuses at the
  cell end, on the battery interface board, rated at least 67.2 V (rule EV.8.4.3).
- **Balancing**: passive, internal FETs. R_CB is 10R 2512 1 W on each side of the
  cell, so I_CB = Vcell / (2 x 10R + Rdson), about 140 to 196 mA (SLV s5 note; the s1
  target says 190 mA). About 3 W per board with 4 channels on.
- **Temperature** (SLV s7): TMPn → GPIOn (TMP1 → GPIO1 pin 61 ... TMP8 → GPIO8
  pin 54). Each has a 3.3k pull-up (R23..R30) to TBIAS, and a 10k series resistor
  (R31..R38) with 0.1 uF to GND (C29..C36). TBIAS is TSREF through diode D2
  NSVR0340. **The sensors are the Enepaq modules' built-in sensors, not NTCs**: the
  s7 table gives 2.44 V at -40 C, 1.86 V at 25 C, 1.51 V at 60 C, 1.30 V at 120 C. The
  note sets OT_THR 31% (1.55 V, about 55 C), uses the UT comparator as an open-wire
  detector, and states that **OTCB never triggers**.
- **Comms** (SLV s8): COMH and COML each through SM91502ALA-E with 1k termination,
  49.9R series, 220 pF, PESD1CAN, 100 pF centre tap. RX pin 52 tied to CVDD, TX pin 53
  NC, **FAULT_N pin 62 NC**, so faults reach the master only over the daisy chain.
- Board GND = module negative (high-voltage potential); nylon mounting (SLV s9).

## 6. Rev B findings

| # | Board | Finding | Source |
|---|---|---|---|
| R1 | TIM | MCU drawn as STM32G474**RCT6** (256 KB, -40 to 85 C); firmware and CLAUDE.md assume RE (512 KB) T3 (-40 to 125 C). Confirm the fitted part. If it is RC, the 512 KB link map is wrong. | TIM s2, s4 U1; `tim.ioc` |
| R2 | TIM | VBAT pin 1 appears drawn as no-connect. The G4 expects VBAT tied to VDD when no battery is used, and PC13..PC15 (CAN_TERM_2, GPIO_7, GNDDETECT) are backup-domain pins. **UNVERIFIED** read. | TIM s4 U1 |
| R3 | TIM + MST | **SPI3 MISO contention.** BQ79600 MISO drives high whenever idle and cannot share a bus without a tri-state buffer. The TIM's IMU (U3) is on the same SPI3 MISO. Every IMU read with a master fitted fights the BQ79600. A mutex does not fix this. Needs a buffer on the master, or a separate SPI. | B600 p.3; TIM s6 R29; MST s5 |
| R4 | TIM | CAN termination is undefined while the MCU is in reset or unprogrammed: AUXIN (PC9, PC13) floats with no pull. Add a pull-down so the default is open. | TIM s4, s5; ADM p.15 |
| R5 | TIM | Before firmware disables UCPD dead-battery, a high on PA9 (T_VCP_TX, floating) enables a 5.1k pull-down on PB6 = FDCAN2_TX, which can overpower the ADM weak TXD pull-up and hold **CAN2 dominant** while in reset, the bootloader or unprogrammed. Transceivers are in normal mode then (STBY floating). **UNVERIFIED on hardware.** | G4 p.72 n6; ADM p.15 |
| R6 | TIM fw | `GPIO_7` (PC14) is a push-pull output in `tim.ioc`. On the BMS master it is SDC_OK_N, an opto output, so the MCU drives against the opto. Must be an input for the BMS build. Likewise GPIO_2, 3, 5, 6 must be inputs. | `tim.ioc`; MST s4, s8 |
| R7 | TIM fw | `GPIO_1` (PA6) inits LOW in `MX_GPIO_Init`. On the BMS master that is BQ79600 nCS, so the bridge is selected at boot. It should init HIGH. | `Core/Src/main.c` MX_GPIO_Init; MST s4 |
| R8 | TIM | D9 in the 12V "TVS" position is a CDBA340L Schottky, not a TVS. | TIM s7 |
| R9 | TIM | Block diagram promises analog RC filters, UART, QEI and high-side-switch on the mezzanine; none is drawn. | TIM s2 vs s7 |
| R10 | MST | **No window watchdog fitted.** The note describes a TPS3430 (window 10.35 to 165.8 ms, CRST 4.7 nF ~15.5 ms) and a 3-input AND, but only a 2-input AND (U4) is drawn and WDT_KICK goes nowhere. The note's "U5 holds CRST low" refers to an ESD diode. | MST s3, s7 |
| R11 | MST | NFAULT has a 100k pull-up, so an unpowered or absent BQ79600 reads "no fault" and does not drop BMS_OK. Only firmware (MCU_BMS_OK) catches a dead bridge. | MST s5, s7; B600 p.3 |
| R12 | MST | BQ79600 pull-up/down resistors partly DNP (R5, R6). The datasheet requires MOSI pulled up ("Don't leave it unconnected") and MISO pulled up. Assignment of R3..R7 to nets **UNVERIFIED**. | MST s5; B600 p.3 |
| R13 | MST | Note/drawing disagreements: daisy-chain caps 100 pF drawn vs 220 pF noted; TVS PESD5V0U2BT drawn vs PESD1CAN noted (the slave uses PESD1CAN, 220 pF); J1/J2 COMH/COML naming swapped between note and symbols; layout note refers to "U1 pins 40-43" (BQ79616 numbering, not BQ79600). | MST s4, s6; SLV s8 |
| R14 | MST | J3-9 carries a wire with no readable net label (lands on TIM1_CH1, PC0). Possibly the intended WDT_KICK. **UNVERIFIED.** | MST s4 |
| R15 | TIM | TERM_TOGGLE drives the TLP172AM LED through 240R from AUXOUT; AUXOUT is specified only at 2 mA (VOH 2.4 V). Check that AUXOUT can supply the photorelay trigger current. **UNVERIFIED** (no TLP172AM datasheet). | TIM s5 R9 D5; ADM p.6 |
| R16 | SLV | TBIAS is TSREF through a diode (D2), so the thermistor divider is not referred to TSREF; ratiometric readings will carry the diode drop. And OTCB cannot trigger. Check the intended temperature scaling. **UNVERIFIED** against the BQ79616 GPIO measurement mode. | SLV s7 |
| R17 | TIM | VBAT_ADC divider (130k total) is tapped before the fuse and permanently loads the battery input (~0.1 mA at 13 V); a fault on it is unfused. | TIM s8 R21 R22 |

## Unverified items, collected

- Which TIM nets carry R24..R26 (three of four IMU SPI lines).
- TIM VBAT pin 1 no-connect (R2).
- Exact termination topology around R10, R23, C16, C17 and D5.
- BQ79600 pull resistor net assignments R3..R7 (R12).
- Master J3-9 net (R14).
- Mezzanine mating orientation (pin n to pin n).
- TPS3430 window and reset timing (from the note only; no part placed, no datasheet).
- Pin states while the ST bootloader runs (AN2606 not available).
- R5 (CAN2 held dominant by dead-battery pull-down) is inferred from datasheets, not measured.
