# CM200DZ CAN interface facts

Research note for issue #4. Answers what the Cascadia Motion (BorgWarner Portland)
software manual defines for controlling a CM200DZ inverter over CAN from the TIM VCU.

## Sources

- **[SUM]** *CM Software Users Manual*, document 0A-0163-04, rev. dated 8/11/2025
  (revision history, Appendix J, p.176). Local copy: `Datasheets/CM Software Users Manual.pdf`
  (untracked). The copy linked from cascadiamotion.com/documentation
  (`.../ugd/b5e2cb_e731a297509a4b2786cea7ef36199b7f.pdf`) was downloaded on 2026-11-10 and is
  **byte-identical** (same MD5), so the local copy is the current published version.
- The separate "CAN PROTOCOL" download on cascadiamotion.com/documentation is a Box share
  (`app.box.com/s/gu8b4utzjsrx17in8vipgfqxpt8fuf1u`) that needs JavaScript and could not be read.
  **Not consulted.** The official DBC was not consulted either. Every fact below comes from SUM.

Page numbers are the manual's **printed** page numbers. The PDF page index is printed + 1.
The manual covers the whole CM family (CM200DX/DZ, CM350DZ, CM350SiC, and others). It does not
describe anything CM200DZ-specific in the CAN chapter, so this note assumes the CAN behaviour is
common to the family.

---

## 1. Bus basics

| Fact | Value | Source |
|---|---|---|
| Interface used | CAN A only. CAN B is reserved | SUM §8, p.79 |
| Frame format | CAN 2.0A (11-bit) or 2.0B (29-bit), selectable. J1939 framing is optional | SUM §8.3, p.83; §8.1, p.80 |
| DLC | **All messages are 8 bytes**. The command message must be sent with DLC = 8 | SUM §8.3, p.83; §8.7, p.90 |
| Byte order | **Little-endian (Intel)** | SUM §8.3, p.83 |
| Termination | CM inverters can terminate the bus only through an external wiring change in the I/O connector (see the hardware manual). The EEPROM termination bit applies to PM units | SUM §8.1, p.80–81 |
| Bus-off | Auto bus-off recovery is enabled. If the controller goes bus-off it restarts by itself | SUM §8.2, p.83 |

## 2. Baud rate (getting to 500 kbit/s)

- `CAN_Bit_Rate_EEPROM_(kbps)`: GUI addr 0x0120, **CAN param addr 0x0093**. The only valid values
  are 125, 250, 500 and 1000. **The default is 250 kbit/s.** A new value takes effect only after
  a power cycle, because the CAN module is configured at init. (SUM §8.1, p.81; Table 41 §A.4, p.112)
- Because the default is 250k, a factory-default inverter has to be set to 500k first. There are
  two ways to do it:
  - over CAN at 250k, by writing a parameter message (§5 below), or
  - with the RMS GUI over the serial COM port (SUM §1.1, p.9–10; §3.2, p.19).

  Either way the inverter must then be power-cycled. This is a one-time commissioning step and
  should not be done by VCU firmware at runtime.
- EEPROM parameters can only be written when the motor is not enabled. Most take effect after a
  power cycle. The manual says that "parameters highlighted in yellow will take immediate effect",
  but **the highlighting is not visible in the PDF text, so which parameters those are is
  unverified.** (SUM §8.8.4, p.95)

## 3. ID offset

- `CAN_ID_Offset_EEPROM`: GUI 0x011D, **CAN param addr 0x008D**, range 0x000–0x7C0.
  **The default is 0x0A0.** The inverter uses a contiguous block from offset+0x00 to offset+0x2F,
  so the default block is **0x0A0–0x0CF**. (SUM §8.1, p.80; Table 41 §A.4, p.111)
- Other ID-related parameters:
  - `CAN_Extended_Msg_ID_EEPROM`: 0x0090, 0 = 11-bit.
  - `CAN_J1939_Option_Active_EEPROM`: 0x00AB.
  - `CAN_OBD2_Enable_EEPROM`: 0x00B1, 0 = off. When enabled, it also responds at 0x7DF, 0x7DF+n
    and 0x7E7+n.
  - `CAN_BMS_Limit_Enable_EEPROM`: 0x00B2. When enabled, the inverter listens on fixed ID 0x202.

  (SUM Table 41, p.111; §8.9, p.96; §8.10, p.96)
- Planning implication: the TIM must keep its own frames out of **offset..offset+0x2F**, and
  0x202 must not be used for anything else if the BMS limit feature is enabled.

## 4. Message table (default offset 0x0A0, 11-bit)

Directions are from the inverter's point of view: **TX** = inverter → VCU, **RX** = VCU → inverter.

"Fast" messages are sent every `CAN_Fast_Msg_Rate_EEPROM_(ms)` (GUI 0x0180 / CAN 0x00EB) and
"Slow" messages every `CAN_Slow_Msg_Rate_EEPROM_(ms)` (0x0181 / 0x00EC). The manual calls them
"typical 100 Hz" and "typical 10 Hz". Setting either rate to 0 disables that whole group. The
transmit loop runs every 3 ms, so the actual period is rounded up to a multiple of 3 ms: a 10 ms
setting gives about 12 ms. **The factory default values of the two rate parameters are not
stated (unverified).** (SUM §8.1, p.82; §8.6.1, p.86)

Each broadcast can also be turned off with a bit in `CAN_ACTIVE_MSGS_EEPROM_(Lo_Word)` (CAN
param 0x0094, data bytes 4–5). The default is all 1s. (SUM §8.6.1, p.85–88)

| ID | Dir | Rate | Name | Lo-word enable bit | Source |
|---|---|---|---|---|---|
| 0x0A0 | TX | Slow | Temperatures #1 | 0x0001 | p.86, Table 53 p.142 |
| 0x0A1 | TX | Slow | Temperatures #2 | 0x0002 | Table 54 p.142 |
| 0x0A2 | TX | Slow | Temperatures #3 & Torque Shudder | 0x0004 | Table 55 p.142 |
| 0x0A3 | TX | Fast | Analog Input Voltages | 0x0008 | Table 56 p.143 |
| 0x0A4 | TX | Fast | Digital Input Status | 0x0010 | Table 58 p.143 |
| 0x0A5 | TX | Fast | Motor Position Information | 0x0020 | Table 59 p.144 |
| 0x0A6 | TX | Fast | Current Information | 0x0040 | Table 60 p.144 |
| 0x0A7 | TX | Fast | Voltage Information | 0x0080 | Table 61 p.144 |
| 0x0A8 | TX | Fast | Flux Information | 0x0100 | Table 62 p.145 |
| 0x0A9 | TX | Slow | Internal Voltages | 0x0200 | Table 63 p.145 |
| 0x0AA | TX | Fast | **Internal States** | 0x0400 | Table 64 p.145–148 |
| 0x0AB | TX | Fast | **Fault Codes** | 0x0800 | Table 65 p.149 |
| 0x0AC | TX | Fast | Torque & Timer Information | 0x1000 | Table 66 p.149 |
| 0x0AD | TX | Fast | Modulation Index & Flux Weakening Output | 0x2000 | Table 67 p.149 |
| 0x0AE | TX | Slow | Firmware Information | 0x4000 | Table 68 p.150 |
| 0x0AF | TX | 100 Hz fixed | Diagnostic Data (only after a diag download is triggered, §6.8) | 0x8000 (the table misprints 0x0800, see Discrepancies) | p.86, p.88, Table 69 p.150 |
| 0x0B0 | TX | 333 Hz fixed (3 ms) | High Speed Message. **Off by default.** It is enabled by clearing Hi-word bit 0, which gives Hi word 0xFFFE | Hi 0x0001 (0 = on) | p.81, p.86, Table 70 p.150 |
| 0x0B1 | TX | Fast | Torque Capability | Hi 0x0002 | p.86, Table 71 p.150 |
| 0x0C0 | RX | VCU-driven, 3 ms to 500 ms | **Command Message** | Hi-word byte 7 bit 7 (do not disable) | §8.7, p.89–90; Table 35 p.100 |
| 0x0C1 | RX | on demand | Parameter Command (read/write) | — | §8.8, Table 29 p.93 |
| 0x0C2 | TX | reply to 0x0C1 | Parameter Response | — | Table 30 p.93 |
| 0x202 | RX | BMS-driven | BMS current limits (fixed ID, not offset). Only used if `CAN_BMS_Limit_Enable` = 1 | — | §8.10, p.96–97 |

### 4.1 Data formats (scaling)

All values below are 16-bit little-endian unless the message table says otherwise. (SUM §8.4, p.84–85)

| Format | Encoding | Unit / LSB |
|---|---|---|
| Temperature | int16 ×10 | 0.1 °C |
| Low Voltage | int16 ×100 | 0.01 V |
| High Voltage | int16 ×10 | 0.1 V |
| Torque | int16 ×10 | 0.1 N·m |
| Current | int16 ×10 | 0.1 A |
| Angle | int16 ×10 | 0.1° |
| Angular Velocity | int16 | 1 rpm |
| Frequency | int16 ×10 | 0.1 Hz |
| Power | int16 ×10 | 0.1 kW |
| Flux | int16 ×1000 | 0.001 Wb |
| Boolean | uint8 | 0/1 |
| Pressure | int16 ×10 | 0.1 psi |

### 4.2 Byte layouts of every message

**0x0A0 Temperatures #1** (Table 53, p.142)
- B0–1: module A temperature.
- B2–3: module B temperature.
- B4–5: module C temperature.
- B6–7: gate driver board temperature.
- All four are Temperature format.

**0x0A1 Temperatures #2** (Table 54, p.142)
- B0–1: control board temperature.
- B2–3: RTD1.
- B4–5: RTD2.
- B6–7: motor hot-spot estimate.
- All four are Temperature format.

**0x0A2 Temperatures #3** (Table 55, p.142)
- B0–1: estimated coolant temperature.
- B2–3: inverter hot-spot estimate.
- B4–5: filtered motor temperature (from the motor temperature sensor).
- B6–7: torque used in shudder compensation. The table labels its format "Temperature", which is
  probably a misprint for Torque.

**0x0A3 Analog Inputs** (Table 56, p.143)
- Six 10-bit fields in Low Voltage format, at bits 0–9, 10–19, 20–29, 32–41, 42–51 and 52–61
  (AIN1–6).
- Table 57 gives a different layout for iM-225 motor firmware.

**0x0A4 Digital Inputs** (Table 58, p.143)
- B0–7 are DIN1–DIN8 as Booleans: Fwd, Rev, Brake, Regen-disable, Ignition, Start, Valet, DIN8.
- The table header says "Bit(s)" but numbers the entries 0–7. **Whether each input is a byte or a
  bit is ambiguous. Check against the DBC (unverified).**

**0x0A5 Motor Position** (Table 59, p.144)
- B0–1: electrical angle (Angle).
- B2–3: **motor speed** (rpm).
- B4–5: electrical output frequency (Frequency).
- B6–7: delta resolver filtered (Angle, ±180°).

**0x0A6 Current** (Table 60, p.144)
- B0–1: phase A current.
- B2–3: phase B current.
- B4–5: phase C current.
- B6–7: **calculated DC bus current**.
- All four are Current format.

**0x0A7 Voltage** (Table 61, p.144)
- B0–1: **DC bus voltage**.
- B2–3: output voltage (peak line-to-neutral).
- B4–5: VAB, or Vd when the inverter is enabled.
- B6–7: VBC, or Vq when the inverter is enabled.
- All four are High Voltage format.

**0x0A8 Flux** (Table 62, p.145)
- B0–1: Vd_ff (Flux).
- B2–3: Vq_ff (Flux).
- B4–5: Id feedback (Current).
- B6–7: Iq feedback (Current).

**0x0A9 Internal Voltages** (Table 63, p.145)
- B0–1: 1.5 V reference.
- B2–3: 2.5 V reference.
- B4–5: 5.0 V reference.
- B6–7: 12 V reference.
- All four are Low Voltage format.

**0x0AA Internal States** (Table 64, p.145–148)

| Byte / bit | Signal | Values |
|---|---|---|
| B0 | INV_VSM_State | 0 Start, 1 Pre-charge Init, 2 Pre-charge Active, 3 Pre-charge Complete, 4 Wait, 5 Ready, 6 Motor Running, 7 Fault ("Blink Fault Code"), 14 Shutdown in Process, 15 Recycle Power |
| B1 | INV_PWM_Frequency | kHz |
| B2 | INV_Inverter_State | 0 Power-on, 1 Stop, 2 Open Loop, 3 Closed Loop, 4 Wait, 5–7 internal, 8 Idle Run, 9 Idle Stop, 10–12 internal |
| B3 bits 0–5 | Relay 1–6 status | 1 = active |
| B4 bit 0 | INV_Inverter_Run_Mode | 0 = Torque, 1 = Speed |
| B4 bit 1 | Self-Sensing Assist enable | — |
| B4 bits 2–4 | INV_ASC_State | 0 Disabled … 4 Active, 5 Complete, 6 Blocked, 7 Suppressed |
| B4 bits 5–7 | INV_Inverter_Discharge_State | 0 Disabled, 1 Enabled-waiting, 2 Speed Check, 3 Active, 4 Completed |
| B5 bit 0 | INV_Inverter_Command_Mode | 0 = CAN, 1 = VSM |
| B5 bits 4–7 | INV_Rolling_Counter | next expected rolling counter value |
| B6 bit 0 | **INV_Inverter_Enable_State** | 1 = enabled |
| B6 bit 1 | Burst model mode | 0 Stall, 1 High speed |
| B6 bit 2 | BMS limiting regen torque | — |
| B6 bit 3 | *not listed in the table (unverified)* | — |
| B6 bit 4 | Limit: motor-temperature derate | — |
| B6 bit 5 | Limit: motor hot spot | — |
| B6 bit 6 | Key-switch start status (latched) | — |
| B6 bit 7 | **INV_Inverter_Enable_Lockout** | 1 = cannot be enabled |
| B7 bit 0 | INV_Direction_Command | 1 = Forward, 0 = Reverse (or stopped when disabled) |
| B7 bit 1 | BMS message being received | — |
| B7 bit 2 | BMS limiting motoring torque | — |
| B7 bit 3 | Limit: max speed | — |
| B7 bit 4 | Limit: inverter hot spot | — |
| B7 bit 5 | Limit: low-speed current limiting | — |
| B7 bit 6 | Limit: coolant derating | — |
| B7 bit 7 | Limit: stall-burst model | — |

**0x0AB Fault Codes** (Table 65, p.149)
- B0–1: POST_Fault_Lo.
- B2–3: POST_Fault_Hi.
- B4–5: Run_Fault_Lo.
- B6–7: Run_Fault_Hi.
- Read each pair as a 32-bit word: POST = Hi:Lo, RUN = Hi:Lo. The bit meanings are in §7 below.

**0x0AC Torque & Timer** (Table 66, p.149)
- B0–1: commanded torque (Torque).
- B2–3: torque feedback, an estimate (Torque).
- B4–7: power-on timer, uint32 in 3 ms counts. It resets at power-up, so it can be used to detect
  an inverter reset.

**0x0AD Modulation & FW** (Table 67, p.149)
- B0–1: modulation index ×100.
- B2–3: flux-weakening output (Current).
- B4–5: Id command (Current).
- B6–7: labelled "Id command" again. This is presumably the **Iq command (unverified)**.

**0x0AE Firmware Info** (Table 68, p.150)
- B0–1: EEPROM version / project code.
- B2–3: software version.
- B4–5: date code mmdd.
- B6–7: date code yyyy.

**0x0B0 High Speed** (Table 70, p.150)
- B0–1: torque command (Torque).
- B2–3: torque feedback.
- B4–5: motor speed (rpm).
- B6–7: DC bus voltage (High Voltage).

**0x0B1 Torque Capability** (Table 71, p.150; §6.1, p.35)
- B0–1: motoring torque available (Torque).
- B2–3: regen torque available (Torque).
- B4–7: not used.
- The inverter recomputes the available torque every 3 ms. The calculation takes into account
  the EEPROM torque limits and the active current limit, which itself depends on PWM frequency,
  bus voltage, stall model, hot spot, coolant temperature and motor temperature.

**0x0C0 Command Message (VCU → inverter)** (Table 28, p.90; Table 35, p.100; Table 17 §7.1.3, p.64; §8.3, p.83–84)

| Byte / bit | Signal | Encoding |
|---|---|---|
| B0–1 | Torque Command | int16, N·m ×10. Positive = motoring, negative = regen. In speed mode this is a feed-forward into the speed regulator |
| B2–3 | Speed Command | int16, rpm. Used only in speed mode. Signed relative to the direction bit |
| B4 (bit 0) | Direction Command | 1 = Forward, 0 = Reverse |
| B5 bit 0 | Inverter Enable | 1 = on, 0 = off |
| B5 bit 1 | Inverter Discharge | 1 = enable active discharge (§7.3) |
| B5 bit 2 | Speed Mode Enable (override) | 1 = run in speed mode while held, if the EEPROM is set to torque mode |
| B5 bits 4–7 | Rolling Counter | 0–15, incrementing |
| B6–7 | Commanded Torque Limit | N·m ×10. 0 = use the EEPROM limits. A value > 0 replaces **both** the motoring and regen limits (firmware ≥ 1953) |

Worked example from the manual (p.83–84), given there in decimal as `44 1 244 1 0 1 0 0`. In hex
that is `2C 01 F4 01 00 01 00 00`: torque 300 (30.0 N·m), speed 500 rpm, direction 0 (Reverse),
enable 1.

**0x0C1 / 0x0C2 Parameter messages** (Tables 29–30, p.93)

0x0C1 (VCU → inverter):
- B0–1: parameter address (uint16).
- B2: R/W, where 0 = read and 1 = write.
- B3: reserved.
- B4–5: data. A 1-byte value goes in B4.
- B6–7: reserved.

0x0C2 (inverter → VCU):
- Echoes the address in B0–1. The address comes back as 0 if it was not recognised.
- B2: 1 if the write succeeded.
- B4–5: data.

Address ranges (Table 31, p.94):
- 0–99: command and monitor parameters.
- 100–499: EEPROM parameters. These are writable only while the motor is not operating.

Useful command parameter: **address 20 = Fault Clear. Writing 0 clears all active faults, in CAN or
VSM mode.** (§8.8.3, p.94)

## 5. Enable / disable sequence and lockout

Preconditions, all set in EEPROM:
- `Inv_Cmd_Mode_EEPROM` (GUI 0x011B / **CAN 0x008F**) = **0 (CAN mode)**. The default is 1 (VSM
  mode). (§8.1, p.79; §A.2, p.104)
- `Run_Mode_EEPROM` (0x0116 / 0x008E) = 0 for torque mode, which is the default. (§A.2, p.104)

`Key_Switch_Mode` has no effect in CAN mode. (§A.2, p.105)

The VSM steps through the following states (SUM §5, p.30–34):
1. **0 Start**: EEPROM load and POST.
2. **1 Pre-charge Init → 2 Pre-charge Active → 3 Pre-charge Complete.** Pre-charge is driven by
   the inverter's own relay outputs unless `Precharge_Bypassed_EEPROM` (0x008C) = 1, in which case
   it goes straight to Wait. Pre-charge faults if the bus is still charging after 3 s, or if it is
   below the under-voltage threshold. (§5.2, p.32; §A.2, p.104)
3. **4 Wait** → **5 Ready**: the inverter is enabled and waits for the inverter-ready flag.
   → **6 Motor Running**.
4. **7 Fault** on any fault.

Lockout and enable rules (SUM §8.7.1, p.91; Table 64 B6.7, p.148):
- **Inverter Enable Lockout.** After power-up the inverter ignores Enable=1 until it has seen at
  least one command message with **Enable = 0**. Lockout status is reported in 0x0AA B6 bit 7.
- **Direction reversal while enabled.** If the direction bit changes while the inverter is enabled,
  the inverter disables **without a fault and re-arms the lockout**. A fresh Enable=0 is then needed
  before re-enabling. Direction must therefore only change while the inverter is disabled.
- **Optional hardware interlock.** `CAN_Inv_Enab_Switch_Active_EEPROM` (0x009F) = 1 means the
  inverter is enabled only if **DIN1 AND** the CAN enable bit are both active. The default is 0
  (DIN1 ignored). (§8.1, p.82; Table 41 p.113)
- **Start sending commands early.** In CAN mode, commands should start before the inverter powers
  up, or within the timeout. Otherwise a Command-Lost fault is latched at power-up and has to be
  cleared. (§8.7, p.90)

Resulting VCU sequence (derived from the rules above). The manual's own "sequence example" in
§8.7.2 lists the assumptions but no step list:
1. From VCU boot, send 0x0C0 periodically with Enable=0, torque 0, direction set and the rolling
   counter running.
2. Wait for VSM_State = 4 (Wait) or later, no faults in 0x0AB, and Lockout = 0 in 0x0AA.
   If a Command-Lost fault was latched at power-up, clear it with a parameter write to address 20
   (data 0) while Enable=0.
3. Set Enable=1 with torque 0. Confirm 0x0AA B6.0 = 1 and VSM_State = 6.
4. Ramp the torque command.
5. To disable, send Enable=0, typically with torque 0 first.

**Do not send Clear Faults while Enable=1**: "the motor may start running based on the mode and
commanded Torque/Speed." (§5.6.2, p.34)

## 6. Command timeout

- `CAN_Command_Message_Active` (GUI 0x011F / **CAN 0x0092**): 1 enables the timeout, 0 disables
  it. **With the timeout disabled, the inverter holds the last received command indefinitely when
  CAN is lost.** (§8.1, p.81; Table 41 p.112) Cascadia recommends enabling it. **The default value
  is not stated in the manual (unverified). Read it back from the unit.**
- `CAN_Timeout_(/3ms)_EEPROM` (GUI 0x016F / **CAN 0x00AC**): range 0–65535, in 3 ms counts.
  **The default is 333, i.e. 999 ms.** This parameter is **configurable.** (§8.1, p.82; Table 41
  p.113; §8.7.2 p.91)
- On timeout the inverter declares RUN fault **"CAN Command Message Lost" (Run fault bit 11,
  0x00000800)** and **disables the motor.** (§8.1, p.81; Table 73, p.154; §8.12, p.99)
- Recommended command rate: "500 milliseconds rate or faster". 10–50 ms gives better control. The
  inverter processes the command every 3 ms, so sending faster than that gains nothing. Some
  limiting functions act only when a command is received, and may misbehave if commands are
  more than 1 s apart. (§8.7, p.90; §8.1, p.79)
- **Rolling counter** (§8.12, p.98–100; Table 41 p.114). This is a second loss/repeat detector,
  and it raises the **same Command Lost fault (bit 11)**:
  - The counter occupies B5 bits 4–7. The inverter expects last value + 1 (mod 16).
  - Each error adds `CAN_Debounce_Up_Count` (CAN 0x00EF, **default 5**).
  - Each correct value subtracts `..._Down_Count` (CAN 0x00F0, default not stated).
  - It faults at `CAN_Debounce_Counter_Max` (CAN 0x00EE, **default 20; 0 disables the
    feature**).
  - Clearing faults resets the counter.
  - **Implication: with the documented defaults, a VCU that does not increment the counter would
    fault within about 4 messages. The VCU should always send a correctly incrementing counter.**
    Whether the delivered unit actually has this feature enabled is **unverified**; read 0x00EE.

## 7. Fault reporting and clearing

- Faults are reported in **0x0AB** as a POST word (32 bit) and a RUN word (32 bit). The VSM goes
  to state 7. (Table 65, p.149; §5.6, p.33)
- POST faults (Table 72, p.151–152) include:
  - HW desat 0x1
  - HW overcurrent 0x2
  - accelerator shorted/open 0x4/0x8
  - current sensor low/high 0x10/0x20
  - module/PCB/GDB temperatures 0x80–0x800
  - 5/12/2.5/1.5 V sense 0x1000–0x80000
  - DC bus high 0x100000
  - DC bus low (<100 V) 0x200000
  - pre-charge timeout 0x400000
  - pre-charge voltage failure 0x800000
  - EEPROM checksum 0x1000000
  - **EEPROM Update Required 0x4000000**
  - gate driver init 0x10000000
  - brake shorted/open 0x40000000/0x80000000
- RUN faults (Table 73, p.153–154):

  | Bit | Fault |
  |---|---|
  | 0x1 | motor over-speed (> `Motor_Overspeed_EEPROM`) |
  | 0x2 | SW over-current |
  | 0x4 | over-voltage |
  | 0x8 | inverter over-temperature |
  | 0x10 / 0x20 | accelerator shorted/open |
  | 0x40 | direction (deactivated) |
  | **0x80** | **inverter response time-out** (not enabled within 2 minutes of the enable command) |
  | 0x100 | HW desat |
  | 0x200 | HW over-current |
  | 0x400 | under-voltage (< `DC_UnderVolt_Thresh`) |
  | **0x800** | **CAN Command Message Lost** |
  | 0x1000 | motor over-temperature |
  | 0x10000 / 0x20000 | brake shorted/open |
  | 0x40000–0x1000000 | module A/B/C, PCB and GDB1–3 over-temperature |
  | 0x2000000 | current sensor |
  | 0x4000000 | gate driver over-voltage |
  | 0x10000000 | HW over-voltage |
  | 0x40000000 | resolver |

- **Clearing faults over CAN:** write parameter **address 20** with data 0 via 0x0C1, then check
  0x0C2. This clears all active faults, POST faults included. (§8.8.3, p.94; §5.6.2, p.34)
- **These faults cannot be cleared this way:**
  - **HW Gate/Desaturation** needs a power cycle. (Tables 72/73, p.151, 153)
  - **EEPROM Update Required** (set after a firmware flash) needs the EEPROM to be re-accepted via
    the GUI. (§5.6.2, p.34)
- Faults also blink the fault LED. POST faults are followed by two quick blinks. (§5.6.1, p.34)
- Clear faults only while the inverter is disabled (see §5).

## 8. Torque mode vs speed mode

- **Torque mode** (default, `Run_Mode_EEPROM` = 0) is "the normal operating mode". Bytes 0–1 of
  0x0C0 are the torque command. (§A.2, p.104; §7.1.1, p.61)
- **Speed mode** is selected by `Run_Mode_EEPROM` = 1, or at runtime with 0x0C0 B5 bit 2. That bit
  switches a torque-mode inverter into speed mode while it is held at 1, and back when it returns
  to 0. Switching works while enabled and only in CAN mode. In speed mode:
  - the speed regulator output is limited by the motor and regen torque limits;
  - B0–1 become a feed-forward that is **not** rate-limited;
  - `Speed_Rate_Limit` (default 100 rpm/s) applies to the speed command.

  (§7.1.1–7.1.3, p.61–65; Table 46 p.125)
- The manual describes speed mode as mostly for testing, marine or transmission use, and warns
  that it can drive the motor to maximum torque. **The VCU should use torque mode.** (§7.1.1–7.1.2,
  p.61)
- Torque sign conventions relative to the direction bit are in §8.7.3, p.92. With Forward selected,
  positive torque gives positive torque feedback. **With Regen_Fade_Speed > 0 (default 200 rpm),
  negative (regen) torque is not allowed once the motor turns opposite to the commanded
  direction.** (§8.7.3, p.92; Table 46 p.125)

## 9. Built-in limits that can back up the VCU's limits

All values below are EEPROM parameters, writable over CAN by their CAN address:

| Parameter (CAN addr) | Effect | Source |
|---|---|---|
| `Motor_Torque_Limit_EEPROM_(Nm)_x_10` (0x0081) | Maximum motoring torque. Active in CAN mode | Table 45, p.120 |
| `Regen_Torque_Limit_EEPROM_(Nm)_x_10` (0x0082) | Maximum regen torque. Active in CAN mode | Table 45, p.120 |
| 0x0C0 B6–7 Commanded Torque Limit | Runtime override of both limits (0 = EEPROM). Firmware ≥ 1953 | Table 28, p.90 |
| `Torque_Rate_Limit_EEPROM_(Nm)_x_10` (0x00A8) | Maximum torque change per 3 ms, 0.1–250 N·m | Table 45, p.121 |
| `IQ_Limit_EEPROM_(A)_x_10` (0x0064), `ID_Limit_EEPROM_(A)_x_10` (0x0065) | q- and d-axis current limits, in A peak | Table 42, p.115 |
| `Max_Speed_EEPROM_(RPM)` (0x0080) | Torque reduced linearly to 0 between Max Speed and Max Speed × 1.02 (default 10000 rpm) | Table 46, p.125; p.126 |
| `Motor_Overspeed_EEPROM_(RPM)` (0x006F) | Over-speed fault | Table 46, p.125 |
| `Break_Speed_EEPROM_(RPM)` (0x007F) | Torque limit scaled by Break_Speed/speed above the break speed. Can be set equal to Max Speed in CAN mode to remove this effect | §A.9, p.124–125 |
| `DC_Volt_Limit` (0x0066), `DC_UnderVolt_Thresh` (0x0068) | DC bus voltage limit; under-voltage fault | Table 43, p.116 |
| `Inv_OverTemp_Limit` (0x0070), `Mtr_OverTemp_Limit` (0x0071) | Over-temperature faults | Table 44, p.117 |
| `Full_Torque_Temp` (0x0073) / `Zero_Torque_Temp` (0x0072) | Linear motor-temperature torque derate | Table 45, p.121 |
| BMS message on 0x202 (`CAN_BMS_Limit_Enable`, 0x00B2) | B0–1 discharge current limit and B2–3 charge current limit, in A. The inverter applies torque ≤ Vdc·I/ω (ω floored at 525 rpm for a 10-pole motor). Also applies in CAN mode. Drops out 1 s after the last message | §8.10, p.96–97 |
| **Hard output-current limit** | **CM200DZ: 450 A RMS**, not configurable. Further derates with switching frequency, DC voltage and coolant temperature: linear derate 45–80 °C, then to 0 A at 100 °C (firmware ≥ 6511) | §6.7 Table 16, p.55; App. H.2, p.168–169 |

**No power (kW) limit parameter** appears in the manual. The BMS current message is the closest
built-in mechanism for limiting power. The 80 kW FSAE power limit therefore has to be enforced
by the VCU. The only inverter-side backup is the 0x202 current limit, and the manual itself says
its estimate does not account for efficiency. (The FSAE limit value itself is outside the scope
of this note.)

## 10. Discrepancies and unverified items

1. **Defaults not stated** in the manual:
   - `CAN_Command_Message_Active`
   - `CAN_Fast/Slow_Msg_Rate`
   - `CAN_Debounce_Down_Count`
   - the actual factory values on our unit

   **Read them back over 0x0C1 before relying on them.**
2. **Rolling counter defaults to enabled** (Max = 20) according to §8.12, but the main command
   table (§8.7, Table 28, p.90) does not show the counter bits at all. Only Table 35 (p.100) does.
   Treat the counter as live and always send a correct value.
3. Diagnostic Data enable bit: the table on p.86 prints 0x0800, which duplicates Fault Codes. The
   byte-5 bit list on p.88 implies 0x8000.
4. 0x0AD bytes 6–7 are labelled "Id Command" twice. The second is presumably Iq.
5. 0x0AA byte 6 bit 3 is not documented.
6. 0x0A4 digital inputs: the table header says bits, but the entries could be read as bytes.
   Verify against the DBC.
7. The rolling counter Down Count GUI address is given as 0x019A on p.99 and 0x01BA on p.114. The
   name is `..._Down_Count` on p.99 and `..._Up_Down` on p.114. The CAN address 0x00F0 agrees
   in both places.
8. `CAN_ACTIVE_MSGS` Hi and Lo words share CAN address 0x0094. The Lo word is in data bytes 4–5
   and the Hi word in bytes 6–7. This means a write uses bytes 6–7 beyond the "Data B4–5" field
   of Table 29. (p.88; Table 41 p.112)
9. J1939 priority is written as "3 (0b001)" on p.80. The example IDs 0x0CFF.... correspond to
   priority 3. This is irrelevant for 11-bit use.
10. The Ready state says it times out after "a specific amount of time" (§5.4, p.33). Run fault
    0x80 gives **2 minutes** (Table 73, p.153).
11. Which EEPROM parameters take effect immediately ("highlighted in yellow", §8.8.4) cannot be
    determined from the text.
12. The separate cascadiamotion.com "CAN Protocol" document (Box link) and the DBC were not
    readable and were not compared.
