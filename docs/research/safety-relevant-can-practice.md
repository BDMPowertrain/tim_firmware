# Safety-relevant CAN practice for an FSAE car

Research note for issue #8. Date: 2026-11-11.

**Question.** Which established techniques make a CAN network defensible for
safety-relevant signals, and which are proportionate here? For each one: what
it catches, what it costs, how it is normally parameterised, and whether it can
be applied to fixed-format third-party messages (inverter, Energy Meter, IMD) or
only to the VCU-to-BMS messages we define.

**Context assumed.** CAN1 is a 500 kbit/s classic-CAN powertrain bus (VCU, BMS,
Cascadia CM200DZ inverter, FSAE Energy Meter, Bender iso175 IMD on J1939 29-bit
IDs). CAN2 is a separate data-acquisition bus. The STM32G474 FDCAN peripheral
is the controller. No ASIL is claimed.

**How to read this.** Sections 1 to 10 hold sourced facts, each with a citation.
Where I derive something from those facts (arithmetic, combining two sources),
I say "derived". **Section 12 is my recommendation**, kept apart from the facts.
Section 13 lists what I could not confirm.

---

## 0. Answer in one table

| Technique | Failure it detects | Cost | Typical parameterisation | Third-party fixed format? |
|---|---|---|---|---|
| Alive / sequence counter | Repetition, loss, delay, incorrect sequence, a frozen sender, blocking ([E2E Table 6.81](#src-e2e)) | 4 bits of payload per frame (P11); a few lines of code per message | 4-bit counter, 0..14, 0xF reserved; `MaxDeltaCounter` (allowed gap) (E2E 6.12.1.1, Table 6.85) | **Only if the vendor's format already carries a counter.** The receiver cannot add one. |
| Data CRC + Data ID | Corruption in the software path, masquerade, incorrect addressing, insertion (E2E Table 6.81) | 1 byte CRC (+4-bit ID nibble in variant 11C); CRC-8 per frame on both ends | CRC-8-SAE J1850, poly 0x1D, covering Data ID (implicit) + payload minus CRC byte (E2E 6.12.1.4); ≤ 8 bytes on CAN (E2E 9.5) | **Only if the vendor's format already carries one.** |
| Receive timeout (deadline) monitoring | Loss of a sender, a dead bus, bus-off of either end, delay beyond tolerance | One timer per monitored message (or per frame ID) | Chosen from the fault-tolerant time interval: `n_FD = t_FD / cycleTime` (E2E 6.19.3); `ComTimeout`, `ComFirstTimeout` (COM 7.3.6) | **Yes.** Needs only the receiver. The main tool for third-party traffic. |
| Safe substitute values + validity state machine | Prevents use of stale/invalid data after a detected fault | Design effort: one defined value per signal, plus the state logic | `REPLACE` (init value), `SUBSTITUTE`, or `NONE` (hold last) (COM SWS_Com_00470/875/500); NODATA/INIT/VALID/INVALID (E2E 6.19) | **Yes.** Receiver-side only. |
| ID priority assignment + worst-case response-time analysis | Deadline misses caused by arbitration and blocking (a design-time check, not a runtime one) | A spreadsheet or script; needs every message's ID, DLC, period, jitter | Davis et al. 2007 eqs (1), (8)-(13), with an error term (18)-(20); assign priorities with Audsley's algorithm or (D−J)-monotonic as a heuristic (§5, §6.1) | **Partly.** The analysis must include third-party frames, but their IDs are fixed, so only our own IDs can be moved around them. |
| Bus-load budgeting | Over-subscription; loss of headroom for error retransmission | Design-time bookkeeping | Utilisation with worst-case stuffing, plus RTA, since "overall bus utilisation is a poor indicator of system schedulability" (Davis 2007 §4.2) | Third-party load must be measured or taken from vendor docs and counted. |
| Error-passive / bus-off handling and recovery policy | A node harming the bus by repeatedly corrupting frames (the protocol isolates it); a local transceiver or wiring fault | Small: ISR + state machine | Protocol thresholds 128 / 256, recovery after 128 × 11 recessive bits (Bosch Part B §8); app-level two-level recovery `BorTimeL1`, `BorTimeL2`, `BorCounterL1ToL2` (CanSM 7.2.23) | Applies to **our own** controller. A third-party node's behaviour in bus-off is fixed by its vendor. |
| Babbling-node containment | A node flooding valid frames | Mostly design/hardware | Protocol confinement does **not** cover a node sending *valid* frames (derived from Bosch Part B §8); mitigation is timeout monitoring of the victims, transceiver dominant timeout, and priority design | Detection of a third-party babbler is possible from the receive side only. |
| Acceptance filtering | Reception of unexpected IDs, Rx-FIFO overload from irrelevant traffic | Free in hardware: G4 FDCAN has 28 standard + 8 extended filter elements | Reject non-matching frames (`RXGFC.ANFS/ANFE`), reject remote frames (`RRFS/RRFE`) | **Yes.** Receiver-side only. |
| Defined startup and shutdown behaviour | Use of data before the first valid frame; spurious timeouts at startup; garbage at power-down | Design effort | `ComFirstTimeout`, E2E NODATA/INIT states, init values (COM 7.3.6, E2E 6.19) | Receiver-side handling: **yes**. Sender-side behaviour of third parties: fixed by vendor. |

---

## 1. What classic CAN already guarantees (so the E2E layer does not repeat it)

- **Error detection in the protocol.** Every node uses bit monitoring, a CRC,
  bit stuffing and frame checks. The Bosch specification states that all global
  errors and all local errors at transmitters are detected, that "up to 5 randomly
  distributed errors in a message are detected", that "burst errors of length less
  than 15" are detected, as is any odd number of errors. Total residual error
  probability for undetected corrupted messages is less than "message error rate
  × 4.7 × 10⁻¹¹".
  [Bosch CAN 2.0, Part B §2 "Basic Concepts", pp. 39-40](#src-bosch)
- **Automatic retransmission.** Corrupted frames are flagged, aborted and
  retransmitted automatically. Recovery time "from detecting an error until the
  start of the next message is at most 31 bit times, if there is no further
  error". [Bosch Part B §2, p. 40](#src-bosch)
- **Static priority.** "The IDENTIFIER defines a static message priority during
  bus access." [Bosch Part B §2, p. 38](#src-bosch). Davis et al. restate this as:
  CAN behaves as fixed-priority **non-pre-emptive** scheduling with the ID as
  priority. [Davis 2011 §I](#src-davis2011)
- **Standard vs extended frames.** The 29-bit frame's top 11 bits (the Base ID)
  define its base priority, and "the Standard Frame prevails the Extended Frame"
  when the Base ID equals the standard frame's ID.
  [Bosch Part B §3.1.1, p. 45](#src-bosch)

What CAN does **not** give you, and AUTOSAR E2E exists to cover: corruption
introduced *outside* the bus (in a sender's or receiver's software, buffers,
gateways), a sender that keeps transmitting stale data, masquerade (the wrong
sender using an ID), and loss of a sender. AUTOSAR E2E's mechanisms are chosen
"to reduce or avoid an allocation of safety requirements" to the communication
stack: E2E "provides all mechanisms internally".
[E2E §6.2, p. 31](#src-e2e)

AUTOSAR also notes the bus CRC is not the same as the E2E CRC: "This CRC
polynomial is different from the CRC-polynomials used by FlexRay, CAN and LIN"
and the CAN/FlexRay CRCs are computed in the controller hardware.
[E2E §6.5.1.3 and Table 6.12, pp. 79, 82](#src-e2e)

---

## 2. Alive counters and sequence counters

**What it detects.** AUTOSAR distinguishes two uses of one counter field
([E2E §6.2, p. 31](#src-e2e)):

- *Alive counter*: "the value checked at the receiver side if it changes at all,
  but correct incrementation is not checked."
- *Sequence counter*: "the value is checked at receiver side for correct
  incrementation."

From comparing each counter value with the previous one, the receiver detects
([E2E §6.2.3.1, pp. 33-34](#src-e2e)): repetition (no new data, or repeated
data); OK (incremented by 1, or by more than 1 but within limits, meaning
"some data lost"); and error ("incremented more than allowed (i.e. too many data
lost)"). The repetition case is the failed alive check; the too-large jump is the
failed sequence check.

Profile 11 maps the counter to these faults: repetition, loss, delay, incorrect
sequence, loss on a subset of receivers, and blocking access to the channel
("loss or timeout"). [E2E Table 6.81, pp. 153-154](#src-e2e)

**How it is parameterised (Profile 11, the CAN profile).**

- Width: 4 bits, explicitly sent. [E2E Table 6.80, p. 153](#src-e2e)
- Range: starts at 0, increments by 1 per send request, wraps from 0x0E to 0;
  "the counter value 0x0F is reserved as a special invalid value".
  [E2E §6.12.1.1, PRS_E2E_00504, p. 156](#src-e2e)
- Position (variants 11A/11C): CRC in byte 0, counter in the low nibble of
  byte 1 (bit offset 8). [E2E §6.12.1, PRS_E2E_00540/00541, pp. 154-155](#src-e2e)
- `MaxDeltaCounter`: "Maximum allowed gap between two counter values of two
  consecutively received valid Data. For example, if the receiver gets Data with
  counter 1 and MaxDeltaCounter is 3, then at the next reception the receiver can
  accept Counters with values 2, 3 or 4."
  [E2E Table 6.85, p. 170](#src-e2e)
- Check results: `OK`, `NONEWDATA`, `ERROR` (CRC, length or Data ID wrong),
  `REPEATED`, `OKSOMELOST`, `WRONGSEQUENCE`.
  [E2E Table 6.84, pp. 169-170](#src-e2e)
- Profile 11 is the recommended profile for new CAN designs: "Profile 1 is a
  Legacy Profile and is only maintained for compatibility reasons. New Projects
  shall use Profile 11." [E2E §6.5, p. 78](#src-e2e)

**Cost.** 4 bits of the 64-bit classic-CAN payload, an increment on the sender,
a compare and a small state struct per received message on the receiver.

**Limits.** The check only works if it is called periodically: "If one of E2E
functions to protect (sender) or to check (receiver) is not called periodically,
then some communication failure modes (loss, delay) may not be detected."
[E2E §4.1.2, p. 16](#src-e2e). With only 15 values, a gap of a multiple of 15
frames is indistinguishable from no gap (derived: arithmetic on the 0..14 range),
so the counter does not replace a timeout.

**Third-party applicability.** A counter is a sender-side field. The receiver
can only check one if the vendor's fixed format already contains one. Whether
the CM200DZ, the Energy Meter or the iso175 messages contain a rolling counter
is **unverified** (vendor documents not read for this note).

---

## 3. Data CRC and Data ID

**What it detects.** Profile 11: the CRC detects corruption and asymmetric
information to multiple receivers; Data ID + CRC detects masquerading,
incorrect addressing and insertion. [E2E Table 6.81, p. 154](#src-e2e)

AUTOSAR's reasoning on CRC errors: "In contrast to errors indicated based on the
evaluation of the counter - CRC-errors are unlikely to be a 'false alarm'", so
"a more stringent reaction upon CRC-errors is adequate", and "the maximum number
of CRC-errors a receiver tolerates shall be limited".
[E2E §6.2.3.3, pp. 34-35](#src-e2e)

**How it is parameterised (Profile 11).**

- CRC: 8 bits, CRC-8-SAE J1850 via `Crc_CalculateCRC8`, explicitly sent.
  [E2E Table 6.80 and §6.12.1.4, PRS_E2E_00508, p. 157](#src-e2e)
  The underlying polynomial is 0x1D (x⁸ + x⁴ + x³ + x² + 1).
  [E2E §6.5.1.3, PRS_E2E_00070, p. 82](#src-e2e)
  Profile 1 uses start value and XOR value 0x00 for legacy compatibility.
  P11 calls the CRC library with its standard SAE-J1850 settings. The library's
  exact start/XOR values for P11 are in the CRC Library SWS, which I did not
  read (**unverified detail**).
- What it covers: with `DATAID_BOTH`, the 16-bit Data ID is "implicitly
  transmitted, by adding first the Data ID low byte, then the Data ID high byte
  before the user data in the CRC calculation". With `DATAID_NIBBLE`, 4 bits of a
  12-bit Data ID are sent in the frame and the low byte is included implicitly.
  "The byte containing the CRC is always omitted from the CRC calculation."
  [E2E §6.12.1.4, PRS_E2E_00505/00506, p. 157](#src-e2e)
- Data IDs "shall be globally unique within the network".
  [E2E §6.12.1.2, PRS_E2E_00507, p. 156](#src-e2e)
- Length limit: on CAN the complete protected data "should not exceed 8 bytes".
  For Profiles 1 and 2, "the Hamming Distance is 2, up to the given lengths. Due
  to 8 bit CRC, the burst error detection is up to 8 bits."
  [E2E §9.5, PRS_E2E_UC_00061, p. 232](#src-e2e)
- Residual risk the application must tolerate: "transmission of one undetected
  erroneous data element in a sequence ... protected with profile 1, 11, 2, 22,
  will not directly lead to the violation of a safety goal".
  [E2E §9.6, PRS_E2E_UC_00170, p. 233](#src-e2e)
- Data ID caution: a CRC-8 over two different 2-byte Data IDs can produce the
  same value, so "some precautions must be taken by the user".
  [E2E §9.6, p. 233](#src-e2e)

**Alternative polynomial.** Profile 2 uses CRC-8 polynomial 0x2F
(x⁸ + x⁵ + x³ + x² + x + 1), start 0xFF, final XOR 0xFF.
[E2E Table 6.28, §6.6](#src-e2e)

**Cost.** One payload byte for the CRC, plus a nibble if the Data ID nibble is
sent. The STM32G4 CRC peripheral has a programmable polynomial register and a
polynomial-size field (`CRC_POL`, `CRC_CR_POLYSIZE`), so an 8-bit polynomial can
be computed in hardware.
[ST CMSIS `stm32g474xx.h` lines 2631-2652](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h)
A 256-byte table in software is the usual alternative (general knowledge, not
sourced).

**Third-party applicability.** Same as the counter: only if the vendor's format
already carries one.

---

## 4. Receive timeout monitoring and how the timeout is chosen

**What it detects.** Loss of the sender, a bus or wiring failure, or bus-off at
either end: anything that stops fresh frames arriving. AUTOSAR: by reading the
currently available data periodically, "by means of the counter, the receiver
can detect loss of communication and timeouts". Both "no new data" and
"repeated counter" count as "an unavailability of valid data that was updated
since the previous cycle". [E2E §6.2.3.2, p. 34](#src-e2e)

The same thing can be done without a counter. AUTOSAR COM performs I-PDU-based
"reception deadline monitoring", which "shall not take the values of the signals
into account": the timer is reset on each successful reception and on expiry is
restarted, so "timeout notification is periodically reported if reception fails
for a longer period".
[COM §7.3.6.1, SWS_Com_00715/00738/00889, pp. 53-55](#src-com)

**How the timeout is chosen.**

- AUTOSAR ties it to the fault-tolerant time interval: "FTTI = tFD + tFR" (fault
  detection time + fault reaction time), then `n_FD = t_FD / cycleTime`, with
  configuration hints such as `WindowSizeValid − MinOkStateValid <= n_FD`, and
  `2 × WindowSizeInit − 2 <= n_FD` if the NODATA→INIT→INVALID path matters.
  [E2E §6.19.3, p. 213](#src-e2e)
- The check function "should be invoked at least once within FTTI".
  [E2E §9.3, PRS_E2E_UC_00325, p. 231](#src-e2e)
- COM has two parameters: `ComFirstTimeout` (from start until the first
  reception; 0 means do not monitor the first reception) and `ComTimeout`
  (thereafter; 0 disables monitoring).
  [COM §7.3.6, SWS_Com_00333/00716, pp. 53-54](#src-com)
- The Profile 11 example configuration uses `MaxNoNewOrRepeatedData = 15`
  cycles. [E2E Table 6.86, p. 171](#src-e2e). That is an example, not a
  recommended value.

**No primary source I read prescribes "timeout = k × period".** AUTOSAR leaves k
to the safety analysis. A lower bound can be **derived** from the response-time
model in §6: consecutive receptions of a periodic message with period T can be
up to `T + (R_max − R_min)` apart, where `R_max` is the worst-case response time
(including queuing jitter J) and `R_min` is at least the frame transmission time.
To tolerate `n` consecutive lost frames without a false timeout, the timeout must
exceed `(n + 1) × T + (R_max − C)`. The upper bound is the FTTI minus the
reaction time.

**Cost.** One timer or timestamp per monitored ID. The G4 FDCAN also has a
hardware timeout counter (`TOCC.ETOC`, `TOCC.TOS`, `TOCC.TOP` and interrupt
`IR.TOO`). [CMSIS lines 4626-4635, 4746-4747](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h)
It is a single counter, so it cannot replace per-message timers. Its select
modes (continuous / Tx event FIFO / Rx FIFO 0 / Rx FIFO 1) come from the M_CAN
design and are **unverified against RM0440**.

**Third-party applicability.** Fully applicable. It needs no cooperation from
the sender, so it is the primary safety mechanism available for inverter,
Energy Meter and IMD traffic.

---

## 5. Safe substitute values

**What AUTOSAR offers.** On a receive timeout, `ComRxDataTimeoutAction` selects
one of three behaviours ([COM §7.3.6.1, pp. 53-54](#src-com)):

- `REPLACE`: replace the signal with its `ComSignalInitValue` (SWS_Com_00470).
- `SUBSTITUTE`: replace it with a separate `ComTimeoutSubstitutionValue`
  (SWS_Com_00875).
- `NONE` (or omitted): keep the last received value (SWS_Com_00500).

The E2E state machine gives data a validity status. Only `E2E_SM_VALID` means
"data can be used". `NODATA` ("No data from the sender is available since the
initialization"), `INIT` ("not yet possible use it") and `INVALID`
("Communication not functioning properly") all mean "data cannot be used".
[E2E §6.19.3.3, Table 6.148, p. 218](#src-e2e). The windows and thresholds
(`WindowSizeValid`, `MinOkStateValid`, `MaxErrorStateValid`, and the `Init`
and `Invalid` equivalents) set how many good and bad checks move between states.
[E2E Table 6.146, p. 217](#src-e2e)

**Who chooses the value.** AUTOSAR leaves it to the application: "The E2E
Supervision itself does not handle detected communication errors ... A general
standardization of the error handling of an application is usually not
possible." [E2E §9.4, p. 231](#src-e2e)

**Third-party applicability.** Fully applicable. Substitution is the receiver's
decision.

---

## 6. ID priority assignment and worst-case response-time analysis

### 6.1 The model and its assumptions

Davis, Burns, Bril and Lukkien (2007) showed that the original Tindell and Burns
(1994) analysis "may provide guarantees for messages that will in fact miss their
deadlines". They also showed the claimed-optimal deadline-monotonic /
(D−J)-monotonic priority ordering is not optimal for CAN.
[Davis 2007, Abstract and §5](#src-davis2007). Use the 2007 revised analysis,
summarised below.

The analysis holds only if each node can "ensur[e] that, at any given time when
arbitration starts, the highest priority message queued at that node is entered
into arbitration". [Davis 2007 §2.2-2.3](#src-davis2007). A FIFO transmit queue
breaks that assumption. Davis, Kollmann, Pollex and Slomka (2011) measured the
effect: in their case study, maximum schedulable utilisation was 84.5 % with
priority queues, 60.1 % with two ECUs using FIFOs, and 35.8 % with all ECUs
using FIFOs. They conclude "we cannot recommend the use of such queues".
[Davis 2011 §VIII](#src-davis2011)

Relevance to the TIM: the G4 FDCAN has only 3 transmit FIFO/queue elements
(`SRAMCAN_TFQ_NBR = 3`)
([HAL `stm32g4xx_hal_fdcan.c` lines 211-216](../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_fdcan.c)).
These can run as a FIFO or as a queue (`FDCAN_TX_FIFO_OPERATION` /
`FDCAN_TX_QUEUE_OPERATION` = `TXBC.TFQM`)
([HAL `stm32g4xx_hal_fdcan.h` lines 583-584](../../Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_fdcan.h)).
On M_CAN-family controllers, "Messages stored in the Tx Queue are transmitted
starting with the message with the lowest Message ID (highest priority)"
([Microchip PIC32CX-BZ6 datasheet §43.6.6.5](#src-mchp-txq)). That is a
different vendor's implementation of the same register set. The equivalent
RM0440 text is **unverified** because the ST site could not be reached. Even in
queue mode, a software FIFO in front of the 3 hardware slots still causes
priority inversion (derived from Davis 2011's model).

### 6.2 Formulas (Davis et al. 2007)

Bit time τ (2 µs at 500 kbit/s). Message m has `s_m` data bytes, period `T_m`,
deadline `D_m ≤ T_m`, and queuing jitter `J_m`. `hp(m)` / `hep(m)` / `lp(m)` are
the higher / higher-or-equal / lower priority sets.

```
(1) Transmission time, worst-case stuffing, includes 3-bit inter-frame space:
      C_m = ( g + 8·s_m + 13 + floor((g + 8·s_m − 1) / 4) ) · τ
      g = 34 (11-bit ID) or 54 (29-bit ID)
    → C_m = (55 + 10·s_m)·τ  (11-bit)      C_m = (80 + 10·s_m)·τ  (29-bit)

(5) Blocking:            B_m = max_{k ∈ lp(m)} C_k

(8) Busy period:         t_m^{n+1} = B_m + Σ_{k ∈ hep(m)} ceil((t_m^n + J_k) / T_k) · C_k ,  t_m^0 = C_m

(10) Instances to check: Q_m = ceil((t_m + J_m) / T_m)

(11) Queuing delay of instance q = 0..Q_m−1:
      w_m^{n+1}(q) = B_m + q·C_m + Σ_{k ∈ hp(m)} ceil((w_m^n + J_k + τ) / T_k) · C_k

(12) R_m(q) = J_m + w_m(q) − q·T_m + C_m         (13) R_m = max_q R_m(q)

Error model (§3.5): with F(t) = max number of errors in an interval t,
(18) E_m(t) = ( 31·τ + max_{k ∈ hep(m)} C_k ) · F(t)
(19) add E_m(t_m^n) to (8);   (20) add E_m(w_m^n + C_m) to (11)
```

Sources: eq (1)-(3) [Davis 2007 §2.1.3](#src-davis2007); eqs (4)-(13)
[§3, §3.1-3.2](#src-davis2007); eqs (18)-(20) [§3.5](#src-davis2007). The 31-bit
error overhead comes from Bosch Part B p. 40 (Davis 2007 footnote 6).

Simpler, pessimistic but safe ("sufficient") tests for `D ≤ T`:
eq (16) uses `max(B_m, C_m)` as the blocking term, and eq (17) uses `B_MAX` (an
8-byte frame) for every message. [Davis 2007 §3.4](#src-davis2007). Eq (17) is
what you get if you assume "any number of low priority messages" may be added
later, and "computes a correct upper bound". [§6.2](#src-davis2007)

Davis 2007 also observes that the receiver can read the frame before the 3-bit
inter-frame space, so subtracting 3τ is valid. Converting to real time should
assume slow clocks for bit time and jitter, and fast clocks for periods and
deadlines. [§2.3](#src-davis2007)

Priority assignment: (D−J)-monotonic "is still a good heuristic", but Audsley's
optimal priority assignment algorithm finds a schedulable order whenever one
exists, trying candidates "in (D-J) order ... with ties broken according to
message length, longest first". [Davis 2007 §5, §6.1](#src-davis2007)

### 6.3 Worked example at 500 kbit/s

Frame times at 500 kbit/s (τ = 2 µs), from eq (1), derived:

| Frame | Bits (worst-case stuffed, incl. IFS) | Time |
|---|---|---|
| 11-bit ID, 0 bytes | 55 | 110 µs |
| 11-bit ID, 4 bytes | 95 | 190 µs |
| 11-bit ID, 8 bytes | 135 | 270 µs |
| 29-bit ID, 8 bytes | 160 | 320 µs |

**The message set below is a hypothetical illustration.** The periods, jitters
and number of inverter broadcasts are placeholders, not the car's real
configuration. Priority is in table order (highest first); all frames are 8
bytes, so the 29-bit IMD frame is lowest only because it was placed there. In
reality its position depends on its Base ID (§1).

| # | Message | ID | T = D | J |
|---|---|---|---|---|
| 1 | VCU → inverter torque command | 11-bit | 10 ms | 0.5 ms |
| 2 | BMS → VCU limits/status | 11-bit | 10 ms | 0.5 ms |
| 3 | VCU → BMS command | 11-bit | 10 ms | 0.5 ms |
| 4-9 | Inverter fast broadcasts (6) | 11-bit | 10 ms | 1 ms |
| 10-14 | Inverter slow broadcasts (5) | 11-bit | 100 ms | 1 ms |
| 15 | Energy Meter | 11-bit | 100 ms | 1 ms |
| 16 | IMD (J1939) | 29-bit | 100 ms | 1 ms |

Utilisation U = Σ C/T = 9 × 270 µs / 10 ms + 6 × 270 µs / 100 ms + 320 µs / 100 ms
= 24.3 % + 1.62 % + 0.32 % ≈ **26.2 %** (derived).

**By hand, message 1 (highest priority).** `hp(1)` is empty.
`B_1 = max C over lp = 320 µs` (the 29-bit frame).
Busy period: `t = B_1 + ceil((t + J_1)/T_1)·C_1 = 320 + 1×270 = 590 µs`.
`Q_1 = ceil((0.59 + 0.5)/10) = 1`, so only one instance needs checking.
`w_1(0) = B_1 = 320 µs`.
`R_1 = J_1 + w_1 + C_1 = 500 + 320 + 270 = 1090 µs`.

**By hand, message 3 (VCU → BMS).** `hp(3) = {1, 2}`, `B_3 = 320 µs`.
`w = 320 + ceil((w + 500 + 2)/10 000)·270 × 2 = 320 + 540 = 860 µs` (converges
in one step). `R_3 = 500 + 860 + 270 = 1630 µs`.

Results for every message, computed with eqs (1), (5), (8), (10)-(13), and with
the error term (18)-(20) using F(t) = ceil(t / T_err) (derived, script-computed):

| Message | R, no errors | R, 1 error / 10 ms | R, 1 error / 1 ms | D |
|---|---|---|---|---|
| 1 VCU→INV torque | 1.09 ms | 1.42 ms | 1.42 ms | 10 ms |
| 2 BMS→VCU | 1.36 ms | 1.69 ms | 2.02 ms | 10 ms |
| 3 VCU→BMS | 1.63 ms | 1.96 ms | 2.29 ms | 10 ms |
| 9 last fast INV bcast | 3.75 ms | 4.08 ms | 5.41 ms | 10 ms |
| 15 Energy Meter | 5.37 ms | 5.70 ms | 7.69 ms | 100 ms |
| 16 IMD | 5.37 ms | 5.75 ms | 8.43 ms | 100 ms |

In every case `Q_m = 1` (the busy period ends before the next instance is
queued), so the original Tindell analysis and the revised analysis agree for
this set. Davis 2007 §4.5 explains why: the original analysis is correct
"for any message where there exists at least one lower priority message with
equal or longer transmission time". That condition holds here because all frames
are 8 bytes and the lowest is longer. It would **not** hold for a short,
low-priority-but-not-lowest message, so run the full analysis on the real set.

Bit-error-rate context: "Bit error rates on CAN are typically very low: 10⁻¹¹ up
to 10⁻⁶ depending on environmental conditions", and "any commercial application
... should include at least a simple error model".
[Davis 2007 §4.3](#src-davis2007). The 1-error-per-ms case above is far beyond
that range and is shown only as a stress case.

**Third-party applicability.** The analysis must include all frames on CAN1,
including third-party ones, but only the team's own IDs (VCU, BMS) can be moved.
The inverter, Energy Meter and IMD IDs and periods are vendor-fixed or
vendor-configurable. Their actual values are **unverified** here.

---

## 7. Bus-load budgeting

- **Count frames at worst-case length.** Use eq (1) (stuffing included), not
  nominal frame length. [Davis 2007 §2.1.3](#src-davis2007)
- **Utilisation alone is not the test.** "For real-world CAN systems,
  characterised by messages with non-zero queuing jitter and consequently
  deadlines less than periods, overall bus utilisation is a poor indicator of
  system schedulability." The original analysis was shown to break at
  utilisations as low as a few per cent for pathological sets.
  [Davis 2007 §4.2, Table 4](#src-davis2007)
- **What is achievable.** With schedulability analysis and good priority
  assignment, CAN systems reach "bus utilisations of up to about 80 %". Before
  analysis, with IDs assigned "simply as a way of identifying the data and the
  sending node", "typically around 30 %" was achievable.
  [Davis 2011 §I](#src-davis2011)
- **Random priority assignment is expensive.** In Davis 2011's case study, a
  priority assignment uncorrelated with deadlines raised the minimum bus speed
  from 277 kbit/s to 731 kbit/s and cut maximum utilisation from 84.5 % to
  32.0 %. [Davis 2011 §VIII](#src-davis2011)
- **Headroom for errors and growth.** Use the error term (18) for errors. For
  growth, budget against the sufficient test eq (17), which assumes worst-case
  blocking from any future low-priority 8-byte frame.
  [Davis 2007 §3.4, §3.5, §6.2](#src-davis2007)

I found no primary source giving a single "maximum bus load %" rule. Any
percentage cap is a team policy (see §12).

---

## 8. Error-passive and bus-off handling, and recovery policy

### 8.1 Protocol rules (Bosch CAN 2.0 Part B §8 "Fault Confinement", pp. 61-63)

- A node has three states: error active, error passive, bus off. An
  error-passive unit sends only passive error flags and "will wait before
  initiating a further transmission". A bus-off unit "is not allowed to have any
  influence on the bus".
- Counters: a receiver error adds 1 to REC (rule 1). A transmitter sending an
  error flag adds 8 to TEC (rule 3). A successful transmission subtracts 1 from
  TEC (rule 7). A successful reception subtracts 1 from REC (rule 8).
- Thresholds: error passive when TEC ≥ 128 or REC ≥ 128 (rule 9). Bus off when
  TEC ≥ 256 (rule 10). Back to error active when both counters ≤ 127 (rule 11).
- Recovery: a bus-off node "is permitted to become 'error active' ... with its
  error counters both set to 0 after 128 occurrence of 11 consecutive 'recessive'
  bits" (rule 12). The word is *permitted*: the protocol does not require
  automatic recovery.
- Bosch note: "An error count value greater than about 96 indicates a heavily
  disturbed bus. It may be of advantage to provide means to test for this
  condition."
- Startup note: if only one node is online, it gets no acknowledgement and "can
  become 'error passive' but not 'bus off' due to this reason". Exception 1 to
  rule 3 is the mechanism.

Derived numbers at 500 kbit/s: from TEC = 0, 32 consecutive failed
transmissions (+8 each) reach bus-off. The minimum recovery wait is
128 × 11 = 1408 bit times = 2.82 ms.

### 8.2 What the STM32G4 FDCAN exposes

From ST's CMSIS header and HAL vendored in this repo:

- `PSR.BO`, `PSR.EP`, `PSR.EW`, `PSR.LEC` (last error code), `PSR.ACT`
  (activity), and the interrupts `IR.BO`, `IR.EP`, `IR.EW`, `IR.PEA`/`PED`
  (protocol error in arbitration / data phase), and `IR.ELO` (error logging
  overflow).
  [CMSIS lines 4656-4686, 4749-4765](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h)
- `ECR.TEC` (8 bits), `ECR.REC` (7 bits), `ECR.RP` (receive error passive),
  `ECR.CEL` (CAN error logging counter).
  [CMSIS lines 4644-4654](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h)
- `CCCR.INIT`, `CCCR.DAR` (disable automatic retransmission), `CCCR.MON` (bus
  monitoring mode), `CCCR.ASM` (restricted operation mode).
  [CMSIS lines 4557-4579](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h);
  HAL modes `FDCAN_MODE_RESTRICTED_OPERATION` and `FDCAN_MODE_BUS_MONITORING`
  [HAL .h lines 549-550](../../Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_fdcan.h)
- The HAL only *reports* bus-off (`ProtocolStatus->BusOff`). It contains no
  automatic recovery.
  [HAL .c line 2489](../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_fdcan.c)

Bus-off behaviour of the M_CAN IP: on bus-off it sets PSR.BO and "sets
CCCR.INIT to stop all CAN operation. To restart CAN operation, the application
software needs to clear CCCR.INIT". After that it waits for the recovery sequence
("at least 128 occurrences of Bus Idle Condition"; the M_CAN User's Manual text
says 129). While recovering it neither receives nor transmits, and a transmission
requested during recovery starts once recovery completes.
[Bosch M_CAN AN004 §2](#src-mcan-an004)

**Unverified.** That the STM32G4 FDCAN is an M_CAN derivative and behaves
exactly like this. The register names match, but I could not retrieve RM0440
to confirm the text. The `PSR.EW` threshold (96 in M_CAN, consistent with the
Bosch note) is also unverified for the G4.

**Consequence (derived).** On the TIM, bus-off is a **latched stop** until
firmware clears `CCCR.INIT`. Recovery policy is therefore a firmware decision
that must be written down, not a default.

### 8.3 Established recovery policy (AUTOSAR CanSM)

AUTOSAR's CAN State Manager specifies a two-level bus-off recovery
([CanSM §7.2.23, ECUC_CanSM_00128-00131](#src-cansm)):

- On bus-off, CanSM reports `CANSM_BSWM_BUS_OFF`, drops ComM to
  `COMM_SILENT_COMMUNICATION`, records a pre-failed DEM event, and restarts the
  controller. (SWS_CanSM_00508/00521/00522/00509)
- Transmission is re-enabled after `CanSMBorTimeL1` (short) while the count of
  consecutive bus-offs without a successful recovery is below
  `CanSMBorCounterL1ToL2`, and after `CanSMBorTimeL2` (long) once it is
  reached. (SWS_CanSM_00514/00515)
- A recovery counts as successful once `CanSMBorTimeTxEnsured` has passed
  without a new bus-off. This time "has to be great enough to ensure that PDUs
  are transmitted again (e. g. time period of the fastest cyclic transmitted
  PDU)". (ECUC_CanSM_00130)
- An optional random delay can be added (`CanSMEnableBusOffDelay`,
  SWS_CanSM_00636).
- If one controller of a network goes bus-off, the others on that network are
  stopped too. (SWS_CanSM_00653)

AUTOSAR gives ranges (0..65.535 s, counter 0..255) but no default values.
Concrete values are a project decision.

**Third-party applicability.** Applies only to our own controllers. A
third-party node's bus-off policy is fixed by its vendor (**unverified** for
each device). The receiver sees it only as a timeout (§4).

---

## 9. Babbling-node containment

- **What the protocol contains.** Fault confinement increments counters only on
  *errors* (Bosch Part B §8 rules 1-6). A node that corrupts frames is driven to
  error passive and then bus off and removed. A node that transmits
  *well-formed* frames too often, or with a wrongly high priority, causes no
  protocol error, so fault confinement does not remove it (derived from Bosch
  Part B §8).
- **What a babbler does to the analysis.** In the RTA model, a babbling
  higher-priority message breaks the minimum inter-arrival assumption (`T_k`)
  for every lower-priority message (derived from Davis 2007 §2.2). The highest
  IDs are exposed least.
- **Hardware help.** The ADM3055E/ADM3057E isolated transceiver datasheet
  describes a dominant-timeout block that protects against bus lock-up in a
  fault condition. This is the stuck-TXD case, not the valid-frame-babbler
  case. Seen only in a search-result summary of the
  [ADM3055E/ADM3057E datasheet](#src-adm); the duration and exact wording are
  **unverified**.
- **Detection from the receive side.** Per-ID receive-rate monitoring (a frame
  arriving much faster than its period) is a simple receiver-side check. I found
  no primary source prescribing it; treat it as a recommendation.
- **Bus guardians.** I wanted to cite the bus-guardian literature
  (e.g. Broster and Burns) but could not retrieve it: **unverified**.

**Third-party applicability.** Detection (rate and timeout monitoring) is
receiver-side, so yes. Prevention inside a third-party node is not under the
team's control.

---

## 10. Acceptance filtering

- Protocol: CAN uses content-addressed messages, and "all nodes in the network
  are able to decide by Message Filtering whether the data is to be acted upon".
  [Bosch Part B §2, p. 38](#src-bosch)
- G4 FDCAN resources: 28 standard-ID and 8 extended-ID filter elements
  (`SRAMCAN_FLS_NBR`, `SRAMCAN_FLE_NBR`), and two Rx FIFOs of **3 elements each**
  (`SRAMCAN_RF0_NBR`, `SRAMCAN_RF1_NBR`).
  [HAL .c lines 211-216](../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_fdcan.c)
- Filter types: range, dual-ID, classic mask, and a reject element
  (`FDCAN_FILTER_RANGE/DUAL/MASK/REJECT`).
  [HAL .h lines 669-683](../../Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_fdcan.h)
- Global filter: non-matching standard and extended frames can be accepted into
  FIFO 0/1 or rejected (`RXGFC.ANFS`, `RXGFC.ANFE`; HAL `FDCAN_REJECT`). Remote
  frames can be rejected separately (`RXGFC.RRFS`, `RXGFC.RRFE`). Each FIFO can
  run in blocking or overwrite mode (`RXGFC.F0OM/F1OM`).
  [CMSIS lines 4897-4919](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h),
  [HAL .h lines 768-770](../../Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_fdcan.h)
- Overflow is visible: `IR.RF0L` ("Rx FIFO 0 Message Lost").
  [CMSIS lines 4707-4708](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h)

**What it protects against (derived).** With only 3 hardware Rx slots per FIFO,
accepting every frame on a bus carrying many inverter broadcasts makes Rx loss
depend on ISR latency. Filtering to the IDs the node needs, and rejecting the
rest, reduces that exposure. It also stops a stray or misconfigured ID from
reaching application code.

**Third-party applicability.** Fully applicable (receiver-side). It is in fact
the only way to choose which third-party frames reach the application.

---

## 11. Startup and shutdown behaviour

- **First reception.** COM can monitor the first reception separately with
  `ComFirstTimeout` (0 disables first-reception monitoring).
  [COM §7.3.6, SWS_Com_00716](#src-com). The E2E state machine starts in
  `NODATA` ("No data from the sender is available since the initialization, data
  cannot be used") and passes through `INIT` before `VALID`.
  [E2E §6.19, Table 6.148](#src-e2e)
- **Counter start.** The Profile 11 sender counter starts at 0 at the first
  transmission request. [E2E PRS_E2E_00504](#src-e2e). A receiver therefore sees
  a counter restart whenever a sender reboots. The state machine's
  `WRONGSEQUENCE` and `INIT` handling covers this.
  [E2E §6.19.1, PRS_E2E_00675/00676](#src-e2e)
- **Lone node at power-up.** A transmitter with no other node to acknowledge it
  goes error passive, not bus off. [Bosch Part B §8 note, p. 63](#src-bosch).
  A VCU that powers up before the inverter or BMS will see ACK errors and
  error-passive status. This is expected and not a fault (derived).
- **Silent / no-communication modes.** CanSM models `NO_COM`, `SILENT_COM` and
  `FULL_COM` network modes, and switches the PDU mode to `CANIF_TX_OFFLINE` in
  silent mode. [CanSM §7.2.15, SWS_CanSM_00541](#src-cansm). On the G4, the equivalent hardware modes are
  restricted operation (`CCCR.ASM`) and bus monitoring (`CCCR.MON`).
  [CMSIS lines 4563-4573](../../Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h)
  Their exact semantics on the G4 are **unverified** without RM0440.
- **Shutdown.** None of the sources I read prescribe a shutdown message
  sequence. The relevant facts are that receivers will see the sender's
  silence as a timeout (§4), and that a node which stops mid-frame produces
  an error frame that the others recover from automatically within 31 bit times
  (Bosch Part B p. 40).

**Third-party applicability.** Receiver-side handling (first-timeout, NODATA,
ignoring data until valid) applies to all senders. Third-party *sender*
behaviour at startup and shutdown (e.g. whether the inverter needs an enable
handshake or lockout clear) is vendor-defined and **unverified** here.

---

## 12. Recommendation: a proportionate baseline for this car

*This section is my recommendation, not a sourced fact. Each item cites the
facts above that support it.*

1. **E2E-protect every message the team defines on CAN1** (VCU↔BMS, and any
   VCU→inverter field the format lets us choose): use AUTOSAR Profile 11
   variant 11C layout (byte 0 CRC-8 SAE J1850 0x1D; byte 1 low nibble 4-bit
   counter 0..14; byte 1 high nibble Data ID nibble), a network-unique 12-bit
   Data ID per message, and `MaxDeltaCounter = 1` or `2`. Following a published
   profile is easier to defend at the Electrical System Form review than a
   home-made scheme, and it costs 2 of 8 bytes. (§2, §3)
2. **Receive-timeout monitor every safety-relevant message on CAN1,
   third-party included.** Set each timeout from the reaction-time budget, not a
   rule of thumb. As a starting point, take `timeout = 3 × period`, then check it
   against the derived lower bound `(n+1)·T + (R_max − C)` (§4) and the
   shutdown-path deadline. Use a separate, longer first-reception timeout.
3. **Explicit validity state per received signal group:** NODATA → VALID →
   INVALID, modelled on the E2E state machine. The torque path uses data only in
   VALID. Substitutes: torque request 0 / inverter disabled; BMS limits =
   most restrictive (0 A discharge and charge). Do not hold the last value for
   anything in the torque or shutdown path. (§5)
4. **Treat a CRC failure more strictly than a counter glitch:** a single
   tolerated CRC error, then INVALID. (E2E §6.2.3.3, §3)
5. **Run the Davis 2007 response-time analysis on the real CAN1 message set**
   (all nodes, real IDs, periods, DLCs, J from task timing), with an error model
   of at least F(t) = 1 + ceil(t / 10 ms). Keep the script in the repo and rerun
   it when a message changes. Give the VCU torque command and the BMS
   limits/fault message the highest IDs the team controls. Check where the
   inverter's fixed IDs and the IMD's 29-bit Base IDs fall relative to them.
   (§6)
6. **Budget bus load** at ≤ 50 % worst-case-stuffed utilisation on CAN1 as a
   team cap, *and* require that the sufficient test eq (17) passes with the
   error model. The 50 % figure is my choice: it leaves margin for errors and
   additions well below the ~80 % ceiling Davis reports for well-assigned
   systems. (§7)
7. **Use the FDCAN Tx queue mode (`TFQM = 1`), not FIFO mode, and feed it from a
   priority-ordered software queue**, so the 3 hardware slots do not cause
   priority inversion. Verify queue-mode ordering on the bench, since the RM0440
   text is unverified. (§6.1)
8. **Bus-off policy, written down:** on `IR.BO`, immediately treat all CAN1
   inputs as INVALID (safe substitutes) and stop the torque request. Clear
   `CCCR.INIT` after a short L1 delay (e.g. 10 ms). After 3 bus-offs without
   `TxEnsured` (e.g. 100 ms) of clean running, switch to an L2 delay (e.g. 1 s)
   and latch a fault that needs a shutdown-circuit reset or power cycle to clear
   before torque is allowed again. Log TEC/REC/LEC and `PSR.EW` transitions
   (TEC/REC > ~96) to CAN2 / SD for diagnosis. This mirrors AUTOSAR CanSM's
   L1/L2 scheme (§8.3); the numbers are placeholders to tune.
9. **Acceptance filters: reject all non-matching IDs and all remote frames on
   CAN1.** Route the torque-path IDs to one Rx FIFO and everything else to the
   other, so a burst of inverter broadcasts cannot push a BMS frame out of a
   3-deep FIFO. Monitor `IR.RF0L/RF1L`. (§10)
10. **Babbler detection:** count frames per accepted ID per window and flag any
    ID arriving at more than ~2× its expected rate. Rely on transceiver dominant
    timeout for stuck-TXD. (§9)
11. **Startup:** expect error-passive while alone on the bus. Do not enable
    torque until every required CAN1 input has reached VALID. **Shutdown:** stop
    the torque request (send zero / disable) for at least one timeout period
    before the VCU stops transmitting, so receivers do not rely on a timeout to
    reach the safe state. (§11)
12. **Keep CAN2 off CAN1's critical path:** separate FDCAN instance (no
    bus-level interaction), lower interrupt priority than FDCAN1, and no CAN1
    code path that blocks on CAN2 transmit space. This is general embedded
    practice, not from a source above.

Not proportionate here, in my view: full AUTOSAR stack adoption, dual-channel
redundancy, and formal bus guardians.

---

## 13. Unverified items and gaps

- **RM0440 (ST STM32G4 reference manual) could not be retrieved**: the ST site
  timed out from this environment. Everything about G4 FDCAN *semantics*
  (bus-off behaviour, Tx queue ordering, `ASM`/`MON` modes, timeout counter
  modes, `PSR.EW` threshold) is inferred from the M_CAN IP documentation (Bosch
  AN004), a Microchip M_CAN-family datasheet, and ST's own HAL/CMSIS source.
  The *existence* of each register field is verified from ST's header. Confirm
  the semantics against RM0440's FDCAN chapter.
- **Device message formats not read**: whether the CM200DZ, the FSAE Energy
  Meter and the Bender iso175 include counters or checksums, their periods, IDs,
  and their own command-timeout behaviour. The CM200DZ inverter's command-message
  timeout in particular should be confirmed from Cascadia's CAN protocol
  document. It is the inverter-side half of the VCU→inverter timeout story.
- **CiA documents, NXP / TI application notes, the Tindell and Burns 1994
  original, and the bus-guardian literature** were not read. The web-search
  quota ran out partway through this research. Tindell and Burns is cited only
  through Davis 2007's account of it.
- **ADM3057E dominant-timeout duration**: not read. The datasheet host was
  unreachable.
- **AUTOSAR CRC Library start/XOR values for Profile 11**: not read. Use the
  CRC Library SWS or a published P11 test vector before implementing.
- **Bosch "data consistency" claim** (Part B p. 38: a message is "simultaneously
  accepted either by all nodes or by no node"): I recall that the literature
  reports a last-bit-of-EOF exception to this, but did not re-verify it.
  It does not change the recommendations, since counters and timeouts cover
  omission anyway.
- The Bosch specification was read from a third-party mirror (Bosch's own
  download link was not found).

---

## Sources

- <a id="src-bosch"></a>**Bosch CAN Specification Version 2.0 (1991)**, Part A
  and Part B. Read from mirror
  `https://customdesignspros.github.io/wp-content/uploads/CAN-specification.pdf`
  (a copy of the Bosch document; Bosch's own hosting not found).
- <a id="src-e2e"></a>**AUTOSAR FO R22-11, E2E Protocol Specification**
  (Document ID 849, AUTOSAR_PRS_E2EProtocol).
  https://www.autosar.org/fileadmin/standards/R22-11/FO/AUTOSAR_PRS_E2EProtocol.pdf
- <a id="src-com"></a>**AUTOSAR CP R22-11, Specification of Communication**
  (Document ID 15, AUTOSAR_SWS_COM), §7.3.6 Deadline Monitoring.
  https://www.autosar.org/fileadmin/standards/R22-11/CP/AUTOSAR_SWS_COM.pdf
- <a id="src-cansm"></a>**AUTOSAR CP R22-11, Specification of CAN State
  Manager** (Document ID 253, AUTOSAR_SWS_CANStateManager), §7.2.23,
  §10.2.4. https://www.autosar.org/fileadmin/standards/R22-11/CP/AUTOSAR_SWS_CANStateManager.pdf
- <a id="src-davis2007"></a>**R. I. Davis, A. Burns, R. J. Bril, J. J. Lukkien,
  "Controller Area Network (CAN) schedulability analysis: Refuted, revisited and
  revised"**, Real-Time Systems 35(3):239-272, 2007. DOI 10.1007/s11241-007-9012-7.
  https://link.springer.com/article/10.1007/s11241-007-9012-7
- <a id="src-davis2011"></a>**R. I. Davis, S. Kollmann, V. Pollex, F. Slomka,
  "Controller Area Network (CAN) Schedulability Analysis with FIFO queues"**,
  ECRTS 2011, pp. 45-56. DOI 10.1109/ECRTS.2011.13.
  https://www-users.cs.york.ac.uk/rd17/papers/CAN-FIFO-ECRTS2011.pdf
- <a id="src-mcan-an004"></a>**Bosch, M_CAN Application Note M_CAN_AN004
  "Busoff Recovery Handling"**, rev 1.0, 2019-11-29.
  https://www.bosch-semiconductors.com/media/ip_modules/pdf_2/m_can/m_can_an004_v1-0_busoff_recovery_handling.pdf
- <a id="src-mchp-txq"></a>**Microchip PIC32CX-BZ6 / PIC32WM-BZ6 datasheet
  DS00005998E, §43.6.6.5 Tx Queue** (M_CAN-style register set).
  https://onlinedocs.microchip.com/oxy/GUID-D1D9CFB7-99CD-47B6-A15F-EF7DD17F80A7-en-US-6/GUID-6B0EFE4B-A7B9-4D1C-AE53-CEBCD2CCF601.html
- <a id="src-st-hal"></a>**STMicroelectronics STM32CubeG4 HAL and CMSIS device
  header**, as vendored in this repository:
  `Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_fdcan.c`,
  `Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_fdcan.h`,
  `Drivers/CMSIS/Device/ST/STM32G4xx/Include/stm32g474xx.h`.
- <a id="src-adm"></a>**Analog Devices ADM3055E/ADM3057E datasheet** (Rev. C per
  search listing), https://www.analog.com/media/en/technical-documentation/data-sheets/adm3055e-adm3057e.pdf.
  Not opened; see §13.
