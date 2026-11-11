# Third-party CAN facts: Energy Meter, iso175, HK-J charger

Research for issue #5. The question: what fixed CAN traffic do the third-party
nodes on the TIM's buses impose, so that the TIM ID plan and BMS/VCU tasks can be
designed around them.

All facts come from the vendor documents in the team's `Datasheets/` folder (not
tracked in git). Citations use these keys:

| Key | Document | Version |
|---|---|---|
| **[EM]** | `EM_User_Manual_10.27.2026kz.pdf`, *Formula SAE Electric Energy Meter Student Manual* | "Updated October 27, 2026", 16 pp. |
| **[ISO]** | `iso175_SAEJ1939_D00415_N_XXEN.pdf`, Bender *iso175 SAE J1939 Specification* (application note) | `iso175_SAEJ1939_D00415_03_N_XXEN / 07.2025`, 12 pp. |
| **[CHG]** | `6.6KW-User-Manual-1.pdf`, ELCON *HK-J Series 6.6KW IP67 Sealed UHF CAN Bus Charger User Manual* | "201611 VER A", 9 pp. |
| **[FSAE]** | `FSAE_Rules_2027_V1-2.pdf`, *Formula SAE Rules 2027* | title page and footers say "Version 1.0, 1 Sept 2026", 147 pp. |

Page numbers are the PDF page, which matches the printed page number in all four
documents. **UNVERIFIED** marks anything the documents do not state. *Inferred*
marks a conclusion drawn from a document's table layout rather than its text.

---

## 1. FSAE Energy Meter

### Bus parameters

- **Bit rate:** 500 kbit/s ("CAN Baud Rate: 500k") [EM p.9, GLV Wiring, "GLV Specifications"]. The manual does not say this is configurable.
- **ID format:** every message is an 11-bit ID [EM pp.10-11, CAN Messaging].
- **Byte order:** "All signals are little endian (Intel)" [EM p.10].
- **Connector:** CAN Low on pin 4 and CAN High on pin 5 of the 7-pin Molex 466230701 base connector. GLV− is pin 6 and GLV+ is pin 7, at 6-38 V and about 125 mA [EM pp.8-9].
- **Termination:** **UNVERIFIED.** The manual does not say whether the meter has an internal 120 Ω terminator. Measure it across pins 4-5 on the competition unit, or ask the organisers. Until then, do not plan on the meter being a bus end.
- **The CAN connection is mandatory:** "Use of the CAN bus connection of the Energy Meter is required ... for ... all teams to provide temperature data" [EM p.10]. Rule EM.3.3.1 says "The Tractive Battery Temperature message ... must be supplied to the Energy Meter CAN bus" [EM p.4]. The Energy Meter manual is incorporated by reference into the rules [FSAE EV.3.2.2-EV.3.2.3, p.93].

### Message table

| ID (hex) | Bits | Direction (seen from the meter) | Rate | Use |
|---|---|---|---|---|
| `0x10D` | 11 | EM transmits | 20 ms | Measurement |
| `0x40D` | 11 | EM transmits | 100 ms | Status |
| `0x60D` | 11 | **EM receives** | "Minimum Rate: 250ms" | **REQUIRED**: Tractive Battery Temperature |
| `0x30D` | 11 | EM receives | not stated | Optional: Team Data 1 |
| `0x30E` | 11 | EM receives | not stated | Optional: Team Data 2 |

Sources: [EM p.10] for 0x10D, 0x40D and 0x60D; [EM p.11] for 0x30D and 0x30E.

### Byte layouts

The signal names and types below come from the manual's tables. The byte spans
come from where the signal labels line up under the "Data Byte" header row
(*inferred*, because the manual does not give bit lengths in text).

**0x10D Measurement** (EM → bus, 20 ms) [EM p.10]

| Bytes | Signal | Type | Unit |
|---|---|---|---|
| 0-3 | Current | `float` (assumed IEEE-754 single precision, LE) | A |
| 4-7 | Voltage | `float` | V |

**0x40D Status** (EM → bus, 100 ms) [EM p.10]

| Byte.bit | Signal | Type |
|---|---|---|
| 0.0 | Violation | bit |
| 0.1 | Logging | bit |
| 0.2 | Fault Active | bit |
| 0.3 | Fault Prev Active | bit |
| 1-4 | Energy | `float`, Wh ("Energy [Whr]") |
| 5-7 | not defined | — |

*Inferred:* the Energy float sits at bytes 1-4. Its label starts under the byte 1
column and a float is 4 bytes. The manual does not define bytes 5-7. The DLC is
**UNVERIFIED** (8 is likely).

**0x60D Temperature: the message the BMS must send** [EM p.10]

| Bytes | Signal | Type | Scaling |
|---|---|---|---|
| 0-1 | Maximum Cell Temp | unsigned, LE | degC = raw × 0.1 |
| 2-3 | Minimum Cell Temp | unsigned, LE | degC = raw × 0.1 |
| 4-5 | Average Cell Temp | unsigned, LE | degC = raw × 0.1 |
| 6-7 | not used | — | — |

- The text is verbatim: "Use: REQUIRED. Received by Energy Meter. Minimum Rate: 250ms. All temperatures are sent as a unsigned with the following scaling. degC = raw * 0.1" [EM p.10].
- *Inferred:* each field is 16 bits (uint16), from the column spans 0-1, 2-3 and 4-5. A uint8 at 0.1 °C would top out at 25.5 °C, so 16-bit is the only width that works. Confirm against a real meter or the organisers' DBC if one is published.
- Because the fields are unsigned, temperatures below 0 °C cannot be represented. Clamp them to 0.
- **Deadline:** send at least once every 250 ms (≥ 4 Hz). Neither the manual nor the rules state a tolerance or the meter's timeout. A 100 ms period leaves a 2.5× margin and costs about 0.3 % of a 500 kbit/s bus (*inferred*).
- The DLC the meter expects is **UNVERIFIED**. Send 8 bytes with bytes 6-7 = 0.

**0x30D / 0x30E Team Data 1 / 2** (optional, received by EM) [EM p.11]

| Bytes | 0x30D | 0x30E |
|---|---|---|
| 0-3 | Team Signal 1, `sint32` LE | Team Signal 3, `sint32` LE |
| 4-7 | Team Signal 2, `sint32` LE | Team Signal 4, `sint32` LE |

No rate is stated. Presumably the meter logs these alongside its own data
(**UNVERIFIED**; the manual only says they provide "vehicle data to the Energy
Meter" [EM p.10]).

### What happens when the expected traffic is missing

- **The meter itself:** the manual describes no on-device reaction to a missing 0x60D, such as a fault bit, a status-LED pattern or a TS interlock. **UNVERIFIED** whether the "Fault Active" bit in 0x40D or the "Idle/Logging with Fault" LED patterns [EM p.12] cover a missing temperature message.
- **Scoring:** "Missing temperature sensor data may be treated as missing data (EV.3.4.2) with official discretion" [EM p.4, EM.3.3.2]. EV.3.4.2 says "Missing Energy Meter data may be treated as a Violation, with official discretion" [FSAE p.93].
- **Penalties for a Violation** [FSAE EV.3.5, p.93]:
  - In Acceleration, Skidpad and Autocross, each run with a Violation DQs the team's best run (EV.3.5.1).
  - In Endurance, each Violation costs 60 s (EV.3.5.2).
  - Repeated Violations can void inspection approval (EV.3.5.3).
  - Data that is not downloaded, or is missing, can also mean no score (D.13.3.4, cited at [EM p.4, EM.4.1.3]).
- **What the temperature is checked against:** "Power and Voltage and temperature limits will be checked by the Energy Meter data" [FSAE EV.3.2.6, p.93]. The cell temperature limit is the lower of the cell datasheet maximum and 60 °C [FSAE EV.8.5.2, p.108]. EV.3.4.1 lists only power (80 kW) and voltage (600 V) as Violation conditions [FSAE p.93]. The rules therefore do not explicitly define what penalty follows a logged over-temperature (**UNVERIFIED**).

### Other behaviour worth knowing

- The meter logs only while it measures TS voltage above 60 V. It opens a new file each time TS rises above 60 V and stops below 60 V [EM p.12]. GLV must be present before and while the TS is active [EM p.3, EM.3.2].
- The meter sits in the Tractive System path, on the switched side of the AIRs, at the most positive or most negative pole and never mid-pack [EM p.7]. Its 0x10D voltage is therefore the post-AIR TS voltage, not the pack voltage.

---

## 2. Bender iso175 IMD (SAE J1939)

### Bus parameters

- **Bit rate:** supported rates are 125, 250, 500, 666, 800 and 1000 kBaud [ISO p.12, §3.1]. The default is **500 kbit/s** [ISO p.7, §2.2, Set index 0x7B: "0x04 - 500 kBit/s (Default)"].
- **Termination:** "Terminating resistance HS-CAN 120 Ω (Jumper)" [ISO p.12, §3.1]. The 120 Ω is selectable by a jumper. The manual does not say where the jumper is or its default state (**UNVERIFIED**).
- **ID format:** 29-bit J1939 identifiers. Bits 28-26 are the priority, then EDP and DP, PDU format, PDU specific, and an 8-bit source address [ISO p.2, §1.1]. Priority is "6 (cyclic), 7 (TP)" [ISO p.2].
- **Source address:** the default is `0xF4` (printed as "244" with a footnote on p.2; the hex value appears on pp.4-7). It can be set to 0x80-0xF7 with Set index 0x7D [ISO p.7]. Neither document mentions J1939 address claim (PGN 60928), so **UNVERIFIED** whether the device does it. Treat the address as static.
- **Byte order:** Intel (little endian) for DataWord and DataDWord values [ISO p.2, §1.2].
- **Reserved 11-bit diagnostic IDs:** "For internal diagnostic purposes, we reserve the right to use the following addresses: 0x400 and 0x400 + diagnostic address (default: 0x401); 0x500 and 0x500 + diagnostic address (default: 0x501). If a message is sent to one of these reserved addresses, the device may respond with a reply to 0x410 or 0x510." Cyclic traffic to these IDs (e.g. every 50 ms) "can result in an overload ... the device may enter an error state ... must therefore be avoided at all costs" [ISO p.2, §1.1]. **No other node on the iso175's bus may use 11-bit IDs 0x400, 0x401, 0x410, 0x500, 0x501 or 0x510.** The doc does not say "11-bit" outright. *Inferred* because all six values fit in 11 bits and none fits the J1939 layout.

### Can it run at 500k alongside 11-bit traffic?

Yes. 500 kbit/s is its default [ISO p.7]. The device's own reserved diagnostic
IDs above show that it already shares the controller with 11-bit traffic
[ISO p.2]. Mixing 11-bit and 29-bit frames on one classic-CAN bus is standard
CAN 2.0B behaviour (ISO 11898-1). That is not stated in the Bender document.

Arbitration (*inferred* from the CAN frame format): a 29-bit frame competes on
its top 11 bits, and an 11-bit frame with the same top 11 bits wins.

- The iso175 cyclic frames `0x18FF0xF4` have top-11 bits `0x63F`.
- The TP frames have top-11 bits `0x73B` (`0x1CECFFF4`) and `0x73A` (`0x1CEBFFF4`).

Every 11-bit ID ≤ `0x63F` therefore outranks the IMD's cyclic data, including all
the Energy Meter IDs. This is harmless at the bus loads involved, but it means IMD
frames are effectively the lowest priority on CAN1.

### Message table (default configuration)

The cyclic message PGNs and cycle times are in [ISO p.3, §1.3.2]. The full CAN
IDs for the cyclic messages are *computed* from priority 6, the PGN and SA 0xF4
using the §1.1 layout. The document does not print them. The TP and request IDs
are printed.

| CAN ID | Bits | Dir (from iso175) | Rate (default) | PGN | Message |
|---|---|---|---|---|---|
| `0x18FF01F4` (computed) | 29 | Tx | **100 ms** | 65281 (0xFF01) | PGN_Info_General |
| `0x18FF02F4` (computed) | 29 | Tx | 0 = off | 65282 (0xFF02) | PGN_Info_IsolationDetail |
| `0x18FF03F4` (computed) | 29 | Tx | 0 = off | 65283 (0xFF03) | PGN_Info_Voltage |
| `0x18FF04F4` (computed) | 29 | Tx | 0 = off | 65284 (0xFF04) | PGN_Info_IT-System |
| `0x1CECFFF4` | 29 | Tx | once, within 2 s of power-on | TP.CM_BAM for 65280 (0xFF00) | DeviceInfo announce (42 bytes, 6 packets) [ISO p.3, §1.3.1] |
| `0x1CEBFFF4` | 29 | Tx | once, 6 packets | TP.DT | DeviceInfo data: device type "iso175" (bytes 0-13), serial number (14-27), part number (28-41) [ISO p.3] |
| `0x18EAFF17` | 29 | Rx | on demand, ≤ 10 Hz | 59904 (Request) | "Read PGN_Info_xx": DLC 3, data = requested PGN LE (`00/01/02/03/04 FF 00`) [ISO p.4, §1.4.1] |
| `0x??EFF4ss` | 29 | Rx | on demand, ≤ 10 Hz | 0xEF00 (PDU1 to DA 0xF4) | PGN_Request for GET, SET and CTL commands: byte 0 = index [ISO pp.4-7] |
| `0x??EFssF4` | 29 | Tx | reply, typ. < 100 ms | 0xEF00 | PGN_Response / error: byte 0 = index (or 0xFF for an error), 7 bytes of data padded with 0xFF [ISO pp.4-5] |

Notes on the table:

- **Request IDs:** `0x18EAFF17` is printed exactly that way, which implies the requester uses SA `0x17`. **UNVERIFIED** whether the device accepts the request from any other SA.
- **Command and response IDs:** the document gives only the PGN "0xEFgg", where gg is the device SA. The priority and the requester SA `ss` are **UNVERIFIED**. Priority 6 (`0x18EF....`) is the likely value.
- **Request rules:** requests must have DLC 3. The rate must not exceed 10 Hz. Wait for each response before sending the next request [ISO p.4, §1.4].
- **Configurability:** with firmware ≥ D720V1.01, each PGN_Info message's PGN can be changed (Set 0x77, low byte 0x01-0xF9) and so can its cycle time (Set 0x79, 0x01-0xFA × 100 ms, 0 = off) [ISO p.7]. "Each request, response and the PGN_Info_xx message must have an unique ID" [ISO p.7].

### Byte layouts

The layouts are from [ISO pp.3-4, §1.3.3]. Units and ranges come from the
matching GET-command entries [ISO pp.8-11, §2.3]. Everything is Intel byte order.

**PGN_Info_General (0xFF01, 100 ms)**

| Bytes | Signal | Scaling / values | Source |
|---|---|---|---|
| 0-1 | R_iso_corrected (negative tolerance shifted) | 0-40500 kΩ, 65535 = SNV | GET 0x4C, p.10 |
| 2 | R_iso_status | 0xFC = estimate during startup, 0xFD = first measured value during startup, 0xFE = normal operation, 0xFF = SNV | GET 0x44, p.9 |
| 3 | Isolation measurement counter | 0-255, increments on each new R_iso | GET 0x36, p.9 |
| 4-5 | Warnings and alarms (bitfield) | see below | GET 0x6C, p.11 |
| 6 | Device activity | 0 = initialization, 1 = normal operation, 2 = self test | GET 0x68, p.11 |
| 7 | N/A | 0xFF | p.3 |

Warnings-and-alarms bits [ISO p.11]:

| Bit | Meaning (true =) |
|---|---|
| 0 | Device error active |
| 1 | HV_pos connection failure |
| 2 | HV_neg connection failure |
| 3 | Earth connection failure |
| 4 | Iso alarm (below the error threshold) |
| 5 | Iso warning (below the warning threshold) |
| 6 | Iso outdated (time since the last measurement ≥ the measurement timeout) |
| 7 | Unbalance alarm |
| 8 | Undervoltage alarm |
| 9 | Unsafe to start |
| 10 | Earthlift open |

**PGN_Info_IsolationDetail (0xFF02, off by default)**

| Bytes | Signal | Scaling |
|---|---|---|
| 0-1 | R_iso_neg | 0-50000 kΩ, 65535 = SNV (GET 0x40, p.9) |
| 2-3 | R_iso_pos | 0-50000 kΩ, 65535 = SNV (GET 0x42, p.9) |
| 4-5 | R_iso_original | 0-50000 kΩ, 65535 = SNV (GET 0x4E, p.10) |
| 6 | Isolation measurement counter | 0-255 |
| 7 | Isolation quality | 0-100 %, 255 = SNV (GET 0x3E, p.9) |

**PGN_Info_Voltage (0xFF03, off by default)**

| Bytes | Signal | Scaling |
|---|---|---|
| 0-1 | HV system voltage | 0.05 V/bit, offset 32128 (−1606.4 V to +1606.35 V), 65535 = SNV (GET 0x5E, p.10) |
| 2-3 | HV_neg to earth | same scaling (GET 0x60, p.10) |
| 4-5 | HV_pos to earth | same scaling (GET 0x62, p.10) |
| 6 | Voltage measurement counter | 0-255 (GET 0x5C, p.10) |
| 7 | N/A | 0xFF |

**PGN_Info_IT-System (0xFF04, off by default)**

| Bytes | Signal | Scaling |
|---|---|---|
| 0-1 | Capacity measured value | 0.1 µF/bit, 1-200, 65535 = SNV (GET 0x52, p.10; the unit is printed "[0.1F]", µF assumed) |
| 2 | Capacity measurement counter | 0-255 |
| 3 | Unbalance measured value | 0-100 % (0 % = HV+, 50 % = HV/2, 100 % = HV−), 255 = SNV (GET 0x2A, p.8) |
| 4 | Unbalance measurement counter | 0-255 |
| 5-7 | N/A | 0xFF |

### Useful configuration and command indices

All of these are sent as PGN_Request 0xEFgg [ISO pp.5-7]. Set commands carry
`Data0 = index, Data1 = AA, Data2 = BB` (parameter = 0xBBAA).

| Index | Command | Values |
|---|---|---|
| 0x6B | Lock | 0xFC = write enable, 0xFD = write disable |
| 0x7B | Baud rate | 0x04 = 500 k (default) |
| 0x7D | Source address | 0x80-0xF7 |
| 0x79 | Cycle time | see the configurability note above |
| 0x47 | Error threshold | 30-2000 kΩ, default 100 |
| 0x4B | Warning threshold | 30-2000 kΩ, default 500 |
| 0x49 | Measurement timeout | 1-64255 s, default 60 |
| 0x31 | Self-holding alarm | 0xFC auto reset / 0xFD latch |
| 0x33 | Reset alarm (CTL) | — |
| 0x57 | Self test (CTL) | — |

Thresholds and timeout values are from the GET table [ISO pp.9-10].

The rules require a response value of at least 500 Ω/V of max TS voltage
[FSAE EV.8.6.3, p.108]. Check the 100 kΩ default error threshold against the
pack voltage: at 600 V the minimum is 300 kΩ.

### What the iso175 does when its expected traffic is missing

The iso175 needs no inbound CAN traffic. It broadcasts PGN_Info_General every
100 ms on its own [ISO p.3]. Requests are optional [ISO p.4]. The document
describes no CAN-silence timeout. Its safety function (opening the shutdown
circuit) must not depend on programmable logic [FSAE EV.8.6.4, p.108], so it
works without CAN. The "Iso outdated" bit (bit 6) is about measurement age, not
bus traffic [ISO p.11].

---

## 3. ELCON HK-J-H650-12 charger (6.6 kW)

### Is it CAN-controlled?

**Yes, optionally. The provided manual does not define the protocol.**

- HK-J-H650-12 is the lithium model of the 540 V / 12 A hardware, with a 170-650 VDC output range and 12 A maximum [CHG p.2, §2].
- "CAN Communication: Optional" [CHG p.3, §3].
- "CAN communication Protection: Automatically stop the output when CAN communication fails" [CHG p.3, §4]. When CAN control is in use, the charger needs a periodic command and turns its output off when the command stops.
- The signal connector carries A = CAN L, B = CAN H, C = CAN GND, D = 12 V+, E = 12 V−, F = Enable. For the >312 V models (which includes the 540 V hardware) see [CHG p.6, §6.4]; for the smaller models see [CHG p.5, §6.2]. The pinout is the same.
- §6.5 "Enable Control" [CHG p.7] has no extractable text and is presumably a figure. The behaviour of the Enable pin, and whether it alone can run the charger without CAN, is **UNVERIFIED**.
- The fault LED code "Red Green Red ... Wrong Communication" [CHG p.7, §7] confirms the charger has a communication fault state.

### Bus parameters

- **Bit rate:** "125Kbps 250Kbps 500Kbps" [CHG p.3, §3]. How the rate is selected (factory order option or setting) and the shipped default are **UNVERIFIED**.
- **Termination:** "Terminal Resistance: NO" [CHG p.3, §3]. The charger has no internal terminator, so the harness must provide both ends.
- **Messages, IDs, ID format, byte layout, rate and timeout:** **UNVERIFIED. None of these appear in the manual.** The CAN timeout length is not stated either.

### Unverified lead (not from the provided sources)

ELCON/TC chargers are widely described as using a separate "CAN protocol"
document. In that document, the BMS sends 29-bit `0x1806E5F4` (requested voltage
and current, 0.1 V and 0.1 A, big-endian, plus a control byte) every 1 s. The
charger replies with `0x18FF50E5` (output V and I plus status flags), and the
charger stops after about 5 s without a command.

**None of this is in `6.6KW-User-Manual-1.pdf`.** It comes from memory of
third-party descriptions and must be confirmed against ELCON's protocol sheet
for this unit, or by sniffing the charger, before any code depends on it. If it
holds, note that the BMS-side source address `0xF4` in that scheme is the same
as the iso175's default SA `0xF4` (see section 4).

---

## 4. Combined fixed-ID table

All IDs these devices occupy, for planning the TIM's own allocation. Rates are
the defaults.

| ID | Bits | Owner | Dir (relative to device) | Rate | Status |
|---|---|---|---|---|---|
| `0x10D` | 11 | Energy Meter | Tx | 20 ms | documented [EM p.10] |
| `0x30D` | 11 | Energy Meter | Rx (team → EM) | team-defined | optional [EM p.11] |
| `0x30E` | 11 | Energy Meter | Rx (team → EM) | team-defined | optional [EM p.11] |
| `0x40D` | 11 | Energy Meter | Tx | 100 ms | documented [EM p.10] |
| `0x60D` | 11 | Energy Meter | **Rx; the BMS must send it** | ≤ 250 ms period | required [EM p.10] |
| `0x400` | 11 | iso175 | reserved (diagnostic Rx) | must not be cyclic | keep free [ISO p.2] |
| `0x401` | 11 | iso175 | reserved (diagnostic Rx, default diag address) | — | keep free [ISO p.2] |
| `0x410` | 11 | iso175 | reserved (diagnostic reply Tx) | — | keep free [ISO p.2] |
| `0x500` | 11 | iso175 | reserved (diagnostic Rx) | must not be cyclic | keep free [ISO p.2] |
| `0x501` | 11 | iso175 | reserved (diagnostic Rx) | — | keep free [ISO p.2] |
| `0x510` | 11 | iso175 | reserved (diagnostic reply Tx) | — | keep free [ISO p.2] |
| `0x18FF01F4` | 29 | iso175 | Tx | 100 ms | ID computed from PGN, priority and SA [ISO pp.2-3] |
| `0x18FF02F4` | 29 | iso175 | Tx | off by default | computed |
| `0x18FF03F4` | 29 | iso175 | Tx | off by default | computed |
| `0x18FF04F4` | 29 | iso175 | Tx | off by default | computed |
| `0x1CECFFF4` | 29 | iso175 | Tx (BAM) | once at power-up | printed [ISO p.3] |
| `0x1CEBFFF4` | 29 | iso175 | Tx (TP.DT) | once at power-up | printed [ISO p.3] |
| `0x18EAFF17` | 29 | (requester) → iso175 | Rx | ≤ 10 Hz, on demand | printed [ISO p.4] |
| `0x18EFF4xx` / `0x18EFxxF4` | 29 | iso175 commands / responses | Rx / Tx | on demand | priority and requester SA UNVERIFIED [ISO pp.4-7] |
| `0x1806E5F4` / `0x18FF50E5` | 29 | HK-J charger | Rx / Tx | ~1 s | **UNVERIFIED, not in the provided manual** |

Combined default bus load (*inferred*, worst-case stuffed frames, about 135 bits
per 8-byte standard frame and 160 per extended frame):

- 0x10D at 50 Hz, 0x40D at 10 Hz, 0x60D at 10 Hz and the iso175 General message at 10 Hz together total about 10 kbit/s.
- That is about 2 % of 500 kbit/s.

### Conflicts and constraints

- **Bit rate:** no conflict at 500 kbit/s.
  - The Energy Meter is fixed at 500 k [EM p.9].
  - The iso175 defaults to 500 k [ISO p.7].
  - The charger supports 500 k among 125/250/500 [CHG p.3]. How it is selected is unverified.
- **ID format:** the bus mixes 11-bit (Energy Meter, iso175 diagnostics) and 29-bit (iso175 J1939, and probably the charger). That is legal on classic CAN 2.0B. The TIM's filters must accept both formats on CAN1.
- **11-bit ID collisions:** the Energy Meter IDs (0x10D, 0x30D, 0x30E, 0x40D, 0x60D) do not collide with the iso175 reserved IDs (0x400, 0x401, 0x410, 0x500, 0x501, 0x510). The TIM allocation must avoid all eleven. 0x30D and 0x30E can be left free or used deliberately for Team Data.
- **Source address 0xF4:** **UNVERIFIED.** If the charger uses the usual ELCON scheme, the BMS→charger frame carries SA `0xF4`, the same as the iso175 default SA. The full 29-bit IDs differ (`0x1806E5F4` against `0x18FF0xF4`), so there is no frame collision. Two nodes claiming one J1939 address is still poor practice, and it only matters if the charger and the IMD share a bus. The iso175 SA can be moved with Set 0x7D [ISO p.7].
- **Termination:** the charger has none [CHG p.3]. The iso175 has a jumper-selectable 120 Ω [ISO p.12]. For the Energy Meter it is unknown. Decide the two bus ends explicitly.
- **Priority:** IMD frames (top-11 bits 0x63F) lose arbitration to any 11-bit ID ≤ 0x63F. That includes all Energy Meter and inverter traffic below that value, which is acceptable for a 100 ms status message.

## Open items to resolve by measurement or by asking the vendor

1. Does the Energy Meter have internal termination? What does it do (status bit or LED) when 0x60D is missing? Is the 0x60D DLC strictly 8? Organisers or measurement.
2. Is the iso175 termination jumper fitted? What is the unit's firmware version (≥ D720V1.01 enables PGN, cycle and baud configuration)? Does it accept a Request from SAs other than 0x17?
3. The HK-J-H650-12 CAN protocol sheet (IDs, layout, timeout), how its bit rate is selected, and the Enable pin behaviour (§6.5 figure). ELCON or sniffing.
4. The Energy Meter manual cites "Formula SAE Rules 2026 Section EV.3.2" [EM p.3, EM.1.2]. The rules provided are the 2027 edition, which keeps the same EV.3.2 numbering. The file is named "V1-2" but its pages say "Version 1.0".
