# BQ79600-Q1 / BQ79616-Q1 timing facts for the 16-device ring

Research for issue #3. Scope: one BQ79600-Q1 bridge on SPI to the TIM, 16
BQ79616-Q1 stack devices at 8 cells each (128S Molicel P50B, 1S4P Enepaq
blocks), transformer-isolated ring. Every number cites its source. Anything
derived by arithmetic is marked **(derived)**. Anything the sources do not
settle is marked **UNVERIFIED**.

## Sources

| Tag | Document | Revision |
|---|---|---|
| DS16 | TI BQ79616-Q1/BQ79614-Q1/BQ79612-Q1 datasheet, SLUSE81F | Rev F, June 2026 |
| DS00 | TI BQ79600-Q1 datasheet, SLUSDS1A | Rev A, Aug 2020 |
| AN00 | TI "BQ79600-Q1 Software Design Reference", SLUAA17A | Rev A, Oct 2023 |
| AN16 | TI "BQ79616-Q1 Software Design Reference", SLVAE86B | Rev B, Aug 2023 |
| SCH-S | BMS Slave schematic (BQ79616 board) | Rev 1.2.0, RELEASED, 2026-10-28 |
| SCH-M | BMS Master schematic (BQ79600 board) | PRELIMINARY, git 45e7b92, 2026-10-20 |
| CELL | Enepaq Li-ion building block datasheet with Molicel P50B | Rev E, 2026-06-16 |

Page numbers are the printed page numbers; for DS16, DS00, AN00 and AN16 they
equal the PDF page index. Schematic references use sheet numbers.

## How the boards are wired (what the timing applies to)

- Each slave measures 8 cells on VC0..VC8 / CB0..CB8. VC9..VC16 and CB9..CB16
  are unused and tied off. VC filter is 100 ohm / 0.47 uF and CB resistor is
  10 ohm with 0.47 uF. VC and CB for a cell are fed from **the same tap net
  (TAP0..TAP8)**. The tap fuses are at the cell end, on the battery interface
  board. [SCH-S sheet 4 layout note 3; sheet 5]
- Temperatures: 8 inputs TMP1..TMP8 go straight to GPIO1..GPIO8 through
  10 k / 0.1 uF RC filters (about 160 Hz). There is **no multiplexer**. The
  bias is "TBIAS" (TSREF through diode D2, about 4.7 V) through 3.3 k. The
  sensors are Enepaq shunt-reference sensors, **not NTCs**. Vout runs from
  2.44 V at -40 C to 1.30 V at 120 C, and the output is the analog OR (hottest)
  of each block's sensors. [SCH-S sheet 7; CELL p.3 Table 5, p.4 Table 6]
- The bridge is a BQ79600-Q1 in SPI mode, with SCLK, MOSI, MISO, nCS and
  SPI_RDY routed to the TIM through the board-to-board connector. BAT is from
  +12 V through 10 ohm and VIO is 3V3. Both COMH and COML go through
  transformers, so the ring is closed. [SCH-M sheets 4-6]
- The BQ79600 NFAULT goes into a hardware AND gate:
  BMS_OK = NFAULT AND MCU_BMS_OK AND WDT_OK, which drives the BMS relay. Any
  NFAULT assertion opens the shutdown circuit directly. [SCH-M sheet 7 design
  note]
- Cell limits: 2.5 V min, 3.6 V typ, 4.2 V max. Working temperature is
  -40..60 C on discharge and 0..60 C on charge. [CELL p.1 Tables 1-2]

## 1. ADC conversion and round-robin timing (per BQ79616)

| Quantity | Value | Source |
|---|---|---|
| Single SAR conversion (Main and AUX) | 8 us | DS16 6.6, p.18 tSAR_CONV |
| Main ADC round robin (24 slots) | 192 us | DS16 6.6, p.18 tMAIN_ADC_CYCLE; 7.3.2.1, p.24 |
| AUX ADC round robin | 192 us | DS16 6.6, p.18 tAUX_ADC_CYCLE |
| ADC timing accuracy (mux, conversions, filters) | +/-1.5 % | DS16 6.6, p.18 tADC_ACC |
| Round robin length with 8 of 16 cells active | still 192 us; unused slots are kept | DS16 7.3.2.1.1.2, p.25, Fig 7-4 |
| GPIOs per main round robin | 1 (one shared GPIO slot) | DS16 7.3.2.1.2.2, p.28 |
| All 8 GPIOs refreshed | 8 RR = 1.536 ms (derived: 8 x 192 us) | DS16 p.28 ("it takes eight round robin cycles") |
| DRDY_MAIN_ADC | set after at least 8 round robins | DS16 7.3.2.1.3.1, p.29 |
| 8-RR single-shot mode | all cells and GPIOs measured once, LPF not effective | DS16 Table 7-2, p.29 |
| AFE settle after entering ACTIVE or changing NUM_CELL | 4 ms | DS16 6.6, p.18 tAFE_SETTLE; p.29 |
| ADC start delay for synchronising the chain | ADC_DLY 0..200 us in 5 us steps | DS16 7.5.4.5.2, p.135 |
| AUX ADC CB-channel AAF settling per channel | 4.3 ms (default), 2.3 ms, 1.3 ms | DS16 7.3.2.2.1.2, p.32; ADC_CONF1, p.134 |
| TSREF enable wait | "wait 1.35 ms" (text) vs tTSREF_ON 6 ms (10-90 %, 1 uF); AN16 says wait 6 ms | DS16 7.3.1.6, p.24; 6.6 p.16; AN16 §8.1 p.7 |

Post-ADC digital LPF for cells (single pole, runs only in continuous mode):
cutoff options are 6.5, 13, 26, 53, 111, 240 and 600 Hz. The register gives
the "average" windows 154, 77, 38, 19, 9, 4 and 1.6 ms. [DS16 7.3.2.1.1.3,
p.26; ADC_CONF1, p.134] The settling-time equation and filter coefficients are
on DS16 p.26. Worked example from the datasheet: a 15 mV step at 26 Hz needs
about 27 ms to settle within 1 LSB. The LPF starts from the first sample rather
than from 0 V. [DS16 p.26]

Data age in continuous mode: an unfiltered VCELL is at most 192 us old. A GPIO
reading is at most about 1.54 ms old (derived), plus the board's 10 k / 0.1 uF
RC (tau about 1 ms, derived from SCH-S sheet 7).

## 2. Communication rates and frame overhead

### Host link (TIM to BQ79600, SPI)

| Quantity | Value | Source |
|---|---|---|
| SCLK range | 2 MHz min, 6 MHz max | DS00 6.6 SPI TIMING, p.8; 7.3.2.1.2.2, p.20 |
| SPI throughput note | a faster SCLK "does not increase the throughput of total system as daisy chain speed still limit[s]" | DS00 p.20 |
| SPI_RDY | mandatory in SPI mode; host checks it before every read or write | DS00 7.3.2.1.2.2, p.19; 7.3.2.1.2.2.1, pp.21-22 |
| Max SPI_RDY service interval t8 | 1 ms at 6 MHz, 890 us at 4 MHz, 550 us at 2 MHz; does not apply if total response is under 256 bytes | DS00 6.6 p.8; note under Fig 7-16, p.23 |
| TX FIFO | two 128-byte ping-pong buffers; RX FIFO 32 bytes | DS00 Fig 7-15, p.22 |
| Read flow | host reads in 128-byte chunks, waiting for SPI_RDY between chunks | DS00 Fig 7-16, p.23 |
| Command frame | nCS low for the whole frame (up to 14 bytes); MOSI held at 1 when idle; MISO idles high (needs a tri-state buffer if the bus is shared) | DS00 p.19-21 |
| UART alternative | 1 Mbps (250 kbps in debug mode), +/-1.5 % | DS00 6.6 p.8; DS16 7.3.6.1.1.1 p.49 |

### Daisy chain (BQ79600 to BQ79616 and stack to stack)

| Quantity | Value | Source |
|---|---|---|
| Half-bit pulse tPW_DC | 250 ns (BQ79600: 230/250/270 ns) | DS16 6.6 p.17; DS00 6.6 p.7 |
| Bit rate on the wire | 2 MHz pulses, 500 ns per couplet | DS16 7.3.6.1.2.2, p.64 |
| Byte format | 13-bit: preamble, 2 SYNC, SOF, 8 data, BERR, postamble | DS16 Table 7-18, p.63 |
| Byte on the wire | up to 8.375 us | DS16 Fig 7-34, p.64; DS00 Fig 7-20, p.25 |
| Byte-to-byte period | 10.875 us at 1 Mbps (set by UART baud) | DS16 Fig 7-34, p.64; DS00 Fig 7-20, p.25 |
| Optional extra response gap | STACK_RESPONSE 0..15.75 us per byte | DS16 7.5.4.3.8, p.128 |
| Re-clock delay per device | 4 to 5 us | DS16 6.6 p.17 tRECLK_DC |
| App-note delay rule after an ADC GO | 192 us + 5 us x number of boards | AN00 §2.3.2 p.7; AN16 §5.2 p.6 |

### Frame overhead

Frame = INIT(1) + [DEV ADR(1), single-device commands only] + REG ADR(2) +
DATA + CRC(2). CRC is CRC-16-IBM, polynomial 0xC002 bit-reversed, initialised
to 0xFFFF. [DS16 7.3.6.1.1.2.1, pp.52-54; DS00 Figs 7-7/7-8, pp.15-16]

- Read command: stack read = 6 bytes; single read = 7 bytes. DATA is one byte
  holding (bytes requested - 1), max 128. [DS00 Fig 7-7, p.15]
- Write command: 1-8 data bytes, so 6..13 bytes (stack/broadcast) or 7..14
  bytes (single). [DS00 Fig 7-7; DS16 Table 7-10, p.52]
- Response: 6 bytes of overhead (INIT, DEV ADR, REG ADR x2, CRC x2) + 1..128
  data bytes, with one frame per device. TI's example is 16 cells = 32 data +
  6 = 38 bytes per device. [DS16 7.3.6.1.1.2.2.2, p.57]
- With a BQ79600 base, **do not use Broadcast Read**: the bridge only returns
  0x00. Use Stack Read or Single Device Read. [DS00 notes under Fig 7-7, p.15]
  (DS16 p.75 and AN16 use broadcast reads; that advice assumes a BQ79616 base.)
- The stack responds top-of-stack first. Device N appends its frame only after
  N+1's frame. If N sees a CRC error in N+1's frame, it does not append its own
  frame. [DS16 p.57; DS00 p.16]
- Host read timeout: tWAIT_READ_MAX = (n-1) x 3 us x 2 + m x 10 us + 100 us.
  Here n is the number of stack devices and m is the total response bytes
  including overhead. [DS00 note under Fig 7-16, p.23]

## 3. Worked estimate: all 128 cells and all temperatures, 16-device ring

Register layout per BQ79616: VCELL8_HI at 0x0578 up to VCELL1_LO at 0x0587
(16 bytes). BUSBAR is at 0x0588 and TSREF at 0x058C-0x058D. GPIO1_HI at
0x058E up to GPIO8_LO at 0x059D (16 bytes). [DS16 7.5.4.6.9, 7.5.4.6.16-19,
7.5.4.6.26, pp.141-147] Invalid addresses in a block read return 0x00. [DS16
7.3.6.1.1.2.1.3, p.53]

**Option A: one stack read from 0x0578, 38 bytes** (8 cells + busbar/gap +
TSREF + 8 GPIOs):

```
command (stack read)        = 6 bytes
response per device         = 6 overhead + 38 data            = 44 bytes
response total m            = 16 x 44                         = 704 bytes
time on chain @ 10 us/B     = 704 x 10 us                     = 7.04 ms   (DS00 tWAIT per-byte term)
time on chain @ 10.875 us/B = 704 x 10.875 us                 = 7.66 ms   (DS16/DS00 byte-to-byte)
command out + propagation   ~ 6 x 10.875 us + 16 x 5 us       ~ 0.15 ms
return propagation          ~ 16 x 5 us                       ~ 0.08 ms
tWAIT_READ_MAX              = 15 x 6 us + 704 x 10 us + 100 us = 7.23 ms
SPI clocking of 704 B       = 0.94 ms @ 6 MHz, 2.8 ms @ 2 MHz (overlaps the chain transfer)
=> one full sweep           ~ 7.3 to 7.9 ms (derived)
```

**Option B: two stack reads** (cells 0x0578, 16 B; GPIOs 0x058E, 16 B): each is
16 x 22 = 352 bytes, about 3.5-3.8 ms, with tWAIT_READ_MAX = 3.71 ms each. The
total is the same 704 bytes, with one more command and no TSREF. Option A is
no slower and also returns TSREF.

Consequences (derived):

- A full cell and temperature sweep takes about 7.5 ms of bus time. Sweeping
  every 10 ms uses about 75 % of the link; every 20 ms uses about 38 %. The
  ceiling is about 125 sweeps/s with nothing else on the bus.
- A FAULT_SUMMARY poll (stack read, 1 byte) is 16 x 7 = 112 bytes: about
  1.1-1.2 ms on the chain, with tWAIT_READ_MAX = 1.31 ms.
- Both options exceed 256 response bytes, so the t8 SPI_RDY service rule
  applies: drain each 128-byte buffer within 1 ms at 6 MHz (550 us at 2 MHz).
  [DS00 p.8, p.23] A 128-byte buffer fills in about 1.3-1.4 ms at 10-10.9 us
  per byte (derived). The SPI read task needs at least that responsiveness, or
  DMA.
- The values read are no fresher than the ADC: cells at up to 192 us old
  (unfiltered) or behind the LPF window, and GPIOs up to 1.54 ms old.

## 4. Wake and auto-addressing from cold

| Step | Time | Source |
|---|---|---|
| BQ79600 power-on to ready for WAKE | 1 ms max | DS00 6.6 p.6 tPOR2COMM |
| WAKE ping low time (BQ79600, MOSI in SPI mode) | 2.5-3 ms; AN00 uses 2.75 ms with nCS low, 2 us guards | DS00 6.6 p.7; AN00 §2.1 p.5 |
| BQ79600 SHUTDOWN to ACTIVE | 2-3.5 ms (AN00: wait at least 3.5 ms) | DS00 6.6 p.7; AN00 §2.1 |
| Then CONTROL1[SEND_WAKE]=1 to wake the stack | one frame | DS00 Table 7-9 step 2, p.27 |
| Per BQ79616: end of received WAKE tone to start of forwarded tone | 6 min, 10 max ms | DS16 6.6 p.16 tSU(WAKE_SHUT) |
| WAKE tone length | 90 pulses x 11-15 us = 1.0-1.35 ms (derived) | DS16 6.6 p.17 nWAKE, tCOMTONE |
| AN00 total wait rule | (1.6 ms tone + 10 ms) x N = 185.6 ms for N=16 | AN00 §2.1 step 4, p.5 |
| AN16 total wait rule (BQ79616 as base) | (10 ms + 0.6 ms) x N = 169.6 ms for N=16 | AN16 §3, p.5 |
| If the bridge was shut down by a SHUTDOWN ping | two WAKE pings needed (COMH/COML RX disabled on first wake) | DS00 note p.12; AN00 note p.5 |

Auto-addressing sequence with the BQ79600 [DS00 Table 7-9, pp.27-28; AN00
§2.2, pp.5-6]: 8 dummy stack writes to 0x343..0x34A (DLL sync), broadcast
write CONTROL1=0x01, 17 broadcast writes DIR0_ADDR=0..16 (0 is the bridge),
broadcast COMM_CTRL=0x02, single write COMM_CTRL=0x03 to device 16, 8 dummy
stack reads of 0x343..0x34A, then a verify stack read of 0x306 and a bridge
read of 0x2001.

- Expect communication faults after the DLL sync and clear them. [DS16
  Table 7-19 step 8, p.67]
- Expect DRST, FCOMM_DET and COMMCLR_DET set after a wake. [DS16 Table 7-35,
  p.106]
- Wait at least 100 us after changing DIR_SEL. [DS00 p.27; DS16 p.65]
- Command timing (derived, not specified by TI): about 28 write frames at about
  0.15 ms each is about 4 ms. 9 stack reads at about 1.1-1.3 ms each is about
  11 ms. Auto-addressing totals roughly 15 ms. **UNVERIFIED**: TI gives no
  figure.

Cold start to first full data set (derived): about 3.5 ms (bridge) + about
186 ms (stack wake, AN00 rule) + about 15 ms (addressing) + 4 ms AFE settle
(overlaps TSREF ramp, up to 6 ms) + 1.54 ms (8 RR for GPIOs) + about 7.5 ms
read. That is **about 0.2 s**. Add LPF settling if the LPF is used.

Waking from SLEEP keeps the configuration if the SLEEPtoACTIVE tone is used:
230 us per device + 30-pulse tone (0.33-0.45 ms), so about 9-11 ms for 16
devices (derived from DS16 6.6 p.16-17). A WAKE tone instead causes a digital
reset. [DS16 7.4.1.3.3, p.108] TI recommends a 1-byte dummy write and read
after SLEEPtoACTIVE. [DS16 7.3.6.1.3.3, p.67] Addresses can be stored in OTP
so that auto-addressing is not needed after a reset; the DLL dummy write and
read are still needed. [DS16 7.3.6.1.3.2.3, p.67]

## 5. Comms faults, broken ring, and comm timeouts

How a fault shows up:

- **Host side**:
  - A read does not complete within tWAIT_READ_MAX, so the host sends a Comm
    Clear and re-reads. [DS00 Fig 7-16, p.23]
  - CRC errors, or fewer than 16 response frames.
  - A frame with a CRC error is still forwarded, so several devices may flag
    the same error. A device below a CRC-bad frame does not append its own.
    [DS16 p.54, p.57; DS00 p.16]
  - Detection latency = time of the failing read + its timeout. That is about
    7.2 ms for a full sweep or 1.3 ms for a FAULT_SUMMARY poll, after the poll
    starts (derived).
- **Device registers**:
  - BQ79616: FAULT_COMM1/2/3 with DEBUG_COMH/COML/UART details for byte, sync,
    BERR and CRC errors. [DS16 Tables 7-23/7-24, p.74; 7.3.6.1.2.2, p.62]
  - BQ79600: FAULT_COMM1/2 (COMH/COML PHY and FRAME, SPI_PHY/FRAME, CRC) and
    debug registers 0x2301-0x2307. [DS00 Fig 7-24, p.30; p.29]
- **Short comm timeout CTS** (alert only, sets FAULT_SYS[CTS]) and **long comm
  timeout CTL** (SLEEP with FAULT_SYS[CTL] set, or SHUTDOWN):
  - Both have options 100 ms, 2 s, 10 s, 1 min, 10 min, 30 min and 1 h.
    Timers reset on any valid frame, count only in ACTIVE, and freeze in SLEEP.
  - BQ79616 default: both **disabled**. [DS16 7.3.6.1.4, pp.69-70;
    COMM_TIMEOUT_CONF 0x0019, pp.127-128]
  - BQ79600 default: **CTS = 10 s, CTL = 1 min to SLEEP** (reset value 0x34).
    Keep CTS shorter than CTL. [DS00 7.3.2.4, p.29; COMM_TIMEOUT 0x2005, p.42]
  - The firmware must keep traffic going, or reprogram this, or the bridge
    will sleep after 1 min of silence.
  - A stack device's CTS fault reaches NFAULT only through the embedded-fault
    path below, which needs working communication. CTS therefore does not give
    the host a timely "ring dead" signal. It is a device-local record.
- **Broken ring recovery**: switch direction with CONTROL1[DIR_SEL]. Devices
  need DIR1_ADDR addresses (auto-address once in the reverse direction). With
  a break, the host talks to the devices below the break with DIR_SEL=0 and
  those above with DIR_SEL=1, so there are two tops-of-stack.
  [DS16 7.3.6.1.3.4, pp.67-69; DS00 7.3.2.3.2 and Table 7-10, pp.28-29;
  AN00 §2.4]
- **UNVERIFIED**: what a stack read returns when the command cannot reach the
  top-of-stack. The text only says each device waits for the frame from the
  device above, so the host should expect a timeout with zero frames, not a
  partial set. Test this on hardware.
- **In SLEEP only**: the heartbeat is a 30-pulse burst every 400 ms (360-440).
  A missing heartbeat is declared after 0.9-1.1 s (HB_FAIL); a burst inside
  200 ms is HB_FAST. This needs the ring so the tone returns to the bridge.
  [DS16 6.6 p.17; DS00 6.6 p.8; DS16 7.3.6.2.3.2, p.77]

## 6. NFAULT: what asserts it and how fast

- The BQ79600 NFAULT is an open-drain output that goes low on any **unmasked**
  fault in the bridge's FAULT_SUMMARY, when NFAULT_EN=1 (reset default 1).
  [DS00 7.3.3.2.1, p.31; DEV_CONF1 reset 0x14, p.41] Masking a group with
  FAULT_MSK also keeps it off NFAULT. [DS00 p.30]
- **Stack faults in ACTIVE**: carried only by the fault bits embedded in
  response frames (FCOMM_EN, default 1 on both parts). Each stack device ORs
  0b111 into the SOF bits of DEV ADR and REG ADR of every response frame it
  forwards. The base asserts NFAULT when at least 2 of 3 bits are set, and
  devices downstream set FCOMM_DET. [DS16 7.3.6.2.3.1, pp.76-77; DS00 Fig 7-25,
  p.31] So **NFAULT for a stack fault in ACTIVE needs a read to be in flight**.
  Latency = hardware detection + wait until the next stack read response passes
  through + frame time.
  - Example with a 10 ms poll (derived): OV up to 8.4 ms + up to 10 ms + up to
    7.7 ms, roughly 26 ms worst case.
  - Contradiction: DS00 text (p.31) says the bridge sets [FTONE_DET] on an
    embedded fault, but its figure and DS16 say FCOMM_DET.
- **In SLEEP**: a faulted device sends a fault tone (90 pulses, repeated every
  50 ms) around the ring. Per-hop latency is 48 us in a stack device
  (tFTS_LATENCY). In the bridge, receipt to NFAULT is 24 us (tFT_LATENCY).
  Detection needs 60 pulses (stack) or 64 pulses (bridge).
  [DS16 6.6 pp.17-18; DS00 6.6 pp.7-8]
  - Lower bound for 16 hops (derived): about 16 x 48 + 24 us = 0.8 ms.
  - **UNVERIFIED** whether the 48 us includes the 60-pulse (about 0.7 ms)
    detection. If it does not, the total is about 12 ms.
- **In SHUTDOWN** (sniff detector enabled): the bridge validates a fault tone
  in VALIDATE mode (entry 10 ms, timeout 150 ms) and drives INH and NFAULT low.
  [DS00 7.3.1.1, p.10; 6.6 p.7] On SCH-M, INH is not used for wake (pin 16
  tied to BAT).
- NFAULT fall time: 100 ns max (BQ79616, 10 k pull-up). [DS16 6.6 p.18]
- Because SCH-M feeds NFAULT straight into BMS_OK, expected faults also open
  the relay unless masked or cleared first. Examples: DRST and COMMCLR_DET
  after a wake, and comm faults from the DLL-sync dummy frames.
  [DS16 Table 7-35 p.106, Table 7-19 step 8 p.67]

## 7. Built-in hardware protectors (BQ79616)

These comparators are independent of the ADC path and run in ACTIVE and
SLEEP. [DS16 7.3.4, p.43; 7.3.4.1.2.1, p.45; 7.3.4.2.2.1, p.47]

| Protector | Threshold range / step | Accuracy | Hysteresis | Cycle (each channel checked once per) | Source |
|---|---|---|---|---|---|
| Cell OV | 2700-3000, 3500-3800, 4175-4475 mV; 25 mV steps (EC table says to 4500) | +/-24 mV (-20..65 C), +/-28 mV (-40..105 C) | 50 mV | tOV_CYCLE 8 ms | DS16 6.5 p.15; OV_THRESH p.158; 6.6 p.18 |
| Cell UV | 1200-3100 mV, 50 mV steps | +/-35 mV (-20..65 C), +/-50 mV (-40..105 C) | 50 mV | tUV_CYCLE 8 ms | DS16 6.5 p.15; UV_THRESH p.158; 6.6 p.18 |
| Thermistor OT | 10-39 % of TSREF, 1 % steps | +/-0.5 % | 2 % | tOT_CYCLE 4 ms | DS16 6.5 p.15; OTUT_THRESH p.159; 6.6 p.18 |
| Thermistor UT | 66-80 % of TSREF, 2 % steps | +/-0.5 % | 2 % | tUT_CYCLE 4 ms | same |
| Comparator timing accuracy | +/-5 % | | | | DS16 6.6 p.18 tHW_COMP_ACC |
| VCB_DONE (balancing stop, uses UV comparator) | 2.45-4.0 V, 25 mV | | | in UV round robin | DS16 p.155 |
| OTCB (balancing pause, uses OT comparator) | 10-24 % TSREF, COOLOFF 4-14 % | | | in OT round robin | DS16 p.156 |

- Worst-case detection latency is one cycle + 5 %: about 8.4 ms for OV/UV and
  4.2 ms for OT/UT (derived). **UNVERIFIED**: no deglitch or consecutive-sample
  count is documented, so a single sample over threshold appears to latch.
- OV/UV channels follow NUM_CELL. UV_DISABLE can exclude individual channels.
  OT/UT apply only to GPIOs set to "ADC and OTUT". TSREF must be on, or all OT
  and UT flags trip. [DS16 pp.43-46]
- BIST: OVUV BIST takes 23 ms (21.8-24.2). During it there are no VCELL
  results and no OV/UV detection, plus 4 ms tAFE_SETTLE afterwards. OTUT BIST
  takes 20 ms (19-21); power BIST 11.5 ms. NFAULT toggles during BIST.
  [DS16 6.6 p.18; Table 7-7 p.44; 7.3.6.4.5.3-4, pp.88-90]
- Against this pack (derived):
  - The lowest OV band starts at 4175 mV. A 4.200 V setting can trip anywhere
    from 4.172 to 4.228 V, which overlaps a normal 4.2 V CV charge end.
    4.225 V (trips 4.197-4.253) or higher avoids nuisance trips.
  - UV at 2.50 V trips between 2.465 and 2.535 V (-20..65 C, +/-35 mV), or
    between 2.45 and 2.55 V (-40..105 C, +/-50 mV).
- Against the Enepaq sensors: SCH-S sheet 7 notes that UT can never see cold
  (sensor max 49 % of TSREF), so it acts as an open-sensor detector. OTCB can
  never trigger (sensor floor 1.30 V, about 26 %). OT at 31 % (1.55 V) is about
  55 C. The 2 % OT hysteresis is 100 mV, about 12 C at the 8 mV/C slope near
  55-60 C (derived from CELL Table 5).

## 8. Open-wire (blown sense fuse) detection

Method [DS16 7.3.6.4.6.4, pp.95-96; DIAG_COMP_CTRL2/3 pp.165-166]:

1. Turn on the current sinks with OW_SNK. VC1-16 and CB1-16 sink, and VC0/CB0
   source, 380/500/600 uA (min/typ/max) [DS16 6.5 p.11].
2. Wait for the external capacitor to discharge ("dV/dt time").
3. Start COMP_ADC_SEL = VC OW (Main ADC, needs continuous mode) or CB OW (AUX
   ADC, continuous mode).
4. The device compares every active VCELL (or AUXCELL) against OW_THR
   (0.5-5 V, 300 mV steps).
5. Wait for DRDY_VCOW or DRDY_CBOW, then turn off the sinks.
6. Read FAULT_COMP_VCOW1/2 and CBOW1/2.

Duration: **TI gives no wait time**; it depends on the board's capacitance.
Estimate for SCH-S (derived, **UNVERIFIED** on hardware):

- With a tap fuse open, the node includes the VC caps (0.47 uF to each
  neighbour) and the CB caps (also 0.47 uF), because VC and CB share the tap
  net. That is about 1.9 uF.
- With only the VC sinks on (380 uA min), the cell voltage drops at about
  0.2 V/ms. Going from 4.2 V to a 1.1 V threshold takes roughly 15 ms. With
  only the VC caps (0.94 uF) it is about 8 ms.
- The time from GO to DRDY_VCOW is not specified (**UNVERIFIED**). If the
  comparison uses the LPF-filtered VCELL, add the LPF window (also
  **UNVERIFIED**; the VCCB check explicitly uses the unfiltered value, p.90).
- After the sinks turn off, the caps recharge through 100 ohm (fast). A
  running LPF needs to re-settle (p.26).
- Side effect (inference): pulling an open node down raises the cell above it,
  so OV or UV comparators can trip during the test. Mask or expect this.

Related ADC-comparison diagnostics:

- VCELL vs AUXCELL check: about number of channels x (AUX settle + 192 us).
  For 8 cells at 4.3 ms that is about 36 ms (derived from DS16 p.91 step 3).
  Thresholds are 6-99 mV.
- GPIO main-vs-AUX check: up to 64 round robins = 12.3 ms (derived from DS16
  p.93).
- CBFET check: balancing must be paused (DS16 pp.93-95).
- Contradiction on the CBFET pass threshold: AUXCELL < 1/2 VCELL in the text
  and figure (p.93-94) vs < 1/3 VCELL in the register description (p.166).

## 9. Measurement accuracy

| Measurement | Accuracy | Source |
|---|---|---|
| Cell, Main ADC, LPF_VCELL=0x03 (53 Hz), 2-4.5 V | -2.2/+1.5 mV at 25 C; -3.0/+2.4 mV (-20..65 C); -3.5/+2.6 mV (-40..105 C and -40..125 C) | DS16 6.5 p.13 VACC_MAIN_CELL |
| Cell, Main ADC, 1-5 V, -40..125 C | -3.7/+2.8 mV | same |
| Cell, AUX ADC, 2-4.5 V | -7.5/+5.4 mV at 25 C up to -9.0/+6.5 mV | DS16 6.5 p.14 |
| Cell LSB | 190.73 uV; Main ENOB 16 bits, AUX 14 bits | DS16 6.5 p.13 |
| GPIO absolute, Main ADC, 0.2-4.6 V, -40..105 C | -5.0/+3.0 mV | DS16 6.5 p.14 VACC_MAIN_GPIO_ABS |
| GPIO/TSREF ratio, Main ADC | +/-0.20 % | DS16 6.5 p.14 |
| GPIO LSB | 152.59 uV | DS16 6.5 p.13 |
| TSREF | 4.975-5.025 V | DS16 6.5 p.12 |
| Die temperature 1 (Main) / 2 (AUX) | +/-3 C / +/-6 C | DS16 6.5 p.15 |
| Main ADC calibration (OTP) | gain +/-0.78 %, offset +/-12.2 mV correction range | DS16 pp.135 |

Temperature with the Enepaq sensor (derived from CELL Table 5 + GPIO absolute
accuracy):

- Sensor slope is about 12 mV/C near 25 C, about 8 mV/C near 55-60 C, and
  about 2 mV/C at 100-120 C.
- The ADC contribution is therefore about +/-0.4 C at 25 C, +/-0.6 C at 60 C
  and +/-2.5 C above 100 C.
- Use absolute volts, not TI's NTC ratiometric formula (SCH-S sheet 7 note 4).
- **UNVERIFIED**: the sensor's own accuracy. CELL Table 6 gives supply,
  current and range but no accuracy figure.

## 10. Balancing constraints

- **Current and thermal**:
  - Designed for up to 240 mA at 75 C ambient. [DS16 7.3.3.1.3, p.39]
  - FET Rdson is 1.45-4.6 ohm. [DS16 6.5 p.13]
  - I_CB = Vcell / (2 R_CB + Rdson). [DS16 7.3.3, p.36]
  - SCH-S uses R_CB = 10 ohm, giving at most about 196 mA. [SCH-S sheet 5 note]
- **Die-temperature pause (CB TWARN)**: balancing on all channels pauses when
  the die exceeds 105 C, and resumes below 95 C (10 C hysteresis). Timers
  freeze while paused. [DS16 6.5 p.13; 7.3.3.1.3 p.39]
- **Other thermal limits**:
  - TWARN is settable at 85/95/105/115 C, +/-5 C, default 105 C.
    [DS16 PWR_TRANSIT_CONF p.127; 6.5 p.11]
  - Thermal shutdown is 130/137/152 C (min/typ/max). [DS16 6.5 p.11]
- **OTCB pause** (thermistor through the OT comparator) needs the OT protector
  already running in round robin. On this board it cannot trigger (§7), so
  board thermal limits fall to firmware and die temperature (SCH-S sheet 7
  note 3).
- **Timers**:
  - Per-channel CB_CELLn_CTRL settings: 0 (stop), 10 s, 30 s, 60 s, 300 s,
    10-120 min in 10-min steps, 150-540 min in 30-min steps, and 600 min.
  - Timer accuracy is +/-5 %.
  - Remaining time is readable in BAL_TIME at 5 s or 5 min resolution.
  - [DS16 p.155; 6.6 p.18; 7.3.3.3.3 p.41; BAL_TIME pp.157-158]
- **Channel rules**:
  - Auto mode alternates odd and even channels with a duty of 5 s, 10 s, 30 s,
    60 s, 5 min, 10 min, 20 min or 30 min.
  - Manual mode allows at most 8 FETs, never 3 consecutive, and no adjacent
    FETs if NO_ADJ_CB=1 (the reset default). An invalid configuration sets
    INVALID_CBCONF and balancing does not start.
  - [DS16 7.3.3.1.2 pp.37-38; BAL_CTRL1 p.156; DEV_CONF reset p.126]
  - Two adjacent FETs change the current (series path). [DS16 p.36]
- **Stop and pause conditions**:
  - Timer expiry, or a cell below VCB_DONE_THRESH (needs the UV protector
    running).
  - FLTSTOP_EN stops all balancing on any unmasked fault.
  - Host pause with CB_PAUSE.
  - Faults during a pause do not terminate balancing.
  - [DS16 Table 7-5 p.41; 7.3.3.3.1 p.41; BAL_CTRL2 p.156]
- **SLEEP**: balancing runs in SLEEP if started in ACTIVE. BAL_ACT can send the
  device to SLEEP or SHUTDOWN when balancing is done; devices may then sit in
  different power states. [DS16 7.3.3.2, p.40]
- **Measurement during balancing**: **UNVERIFIED**. DS16 does not say whether
  VCELL accuracy holds while CB FETs are on. The device has separate VC and CB
  pins and filters, so balancing current does not flow through R_VC
  [DS16 Fig 7-13, p.37]. On SCH-S, however, VC and CB share one tap wire and
  fuse per cell. Inference: I_CB x (harness + fuse resistance) appears as an
  error in that cell's reading and its neighbour's, about 2 mV per 10 mohm at
  196 mA. Pausing balancing (CB_PAUSE) before a precision read avoids this.
  TI requires the pause for the CBFET diagnostic. [DS16 p.94]

## 11. Fault-detection mechanisms and latency

| Mechanism | Where it runs | What it catches | Latency to the TIM | Source |
|---|---|---|---|---|
| Firmware-polled VCELL/GPIO (stack read) | Firmware | Cell V and temperature against software limits | poll period + about 7.3-7.9 ms read + ADC age (192 us cells unfiltered or LPF window; 1.54 ms GPIO) | §1, §3 (derived) |
| Firmware-polled FAULT_SUMMARY | Firmware | Any latched device fault | poll period + about 1.2-1.3 ms | DS16 7.3.6.2.1 p.73; §3 (derived) |
| HW OV/UV comparator | Device hardware | Cell OV/UV | detection at most 8.4 ms; then via poll, or via NFAULT on the next read response | DS16 6.6 p.18; §6 |
| HW OT/UT comparator | Device hardware | GPIO sensor OT (and UT = open sensor here) | detection at most 4.2 ms; then as above | DS16 6.6 p.18 |
| NFAULT, bridge's own faults | BQ79600 hardware | Bridge supply, thermal, comm, FIFO faults (unmasked) | immediate (no latency figure given; **UNVERIFIED**) | DS00 7.3.3.2.1 p.31 |
| NFAULT, stack faults in ACTIVE | Embedded bits in response frames | Any unmasked stack fault | needs a read in flight: up to poll period + frame time, about 26 ms at a 10 ms poll (derived) | DS16 pp.76-77; DS00 p.31 |
| NFAULT, fault tone in SLEEP | Tone around the ring | Unmasked faults active in SLEEP (OV/UV/OT/UT if protectors on, supplies, TWARN, OTP CRC) | about 0.8 ms (48 us/hop + 24 us) to about 12 ms (**UNVERIFIED** interpretation); repeats every 50 ms | DS16 6.6 pp.17-18; p.77 |
| Heartbeat loss (SLEEP) | Tone receivers | Broken cable or dead device while asleep | 0.9-1.1 s | DS16 6.6 p.17; DS00 6.6 p.8 |
| Host read timeout / CRC | Firmware | Comms break, corrupted frame | tWAIT_READ_MAX of the failing read (1.3-7.2 ms) | DS00 p.23 |
| CTS / CTL comm timeout | Device timers | No valid frame for 100 ms .. 1 h | at least 100 ms; device-local record, not a timely host alarm | DS16 pp.69, 127-128; DS00 p.42 |
| Thermal warning / shutdown | Device hardware | Die temperature | TWARN flag; TSHUT 130-152 C forces SHUTDOWN | DS16 6.5 p.11; 7.3.6.4.2 p.84 |
| GPIO8 fault input | Device hardware | External active-low fault | sets FAULT_SYS[GPIO] and NFAULT (not used on SCH-S) | DS16 7.3.5 p.48 |
| Open-wire check | Firmware-initiated | Blown tap fuse, broken sense wire | about 8-15 ms wait (derived estimate) + comparison (**UNVERIFIED**) | §8 |
| VCELL-vs-AUX / GPIO-vs-AUX checks | Firmware-initiated | ADC path faults | about 36 ms / at most 12.3 ms | DS16 pp.91, 93 (derived) |
| OVUV / OTUT / power BIST | Firmware-initiated | Protector path faults | 23 ms / 20 ms / 11.5 ms | DS16 6.6 p.18 |

## Contradictions and gaps found

1. TSREF wait: DS16 p.24 says 1.35 ms. The timing table gives tTSREF_ON = 6 ms
   (1 uF), and AN16 says 6 ms. Use 6 ms.
2. OV range: the EC table says up to 4500 mV; the OV_THRESH register says up
   to 4475 mV.
3. CBFET pass criterion: 1/2 VCELL (DS16 p.93-94) vs 1/3 VCELL (p.166).
4. DS16 COMM_TIMEOUT_CONF says CTS sets "FAULT_SYS[CTL]" (p.128). §7.3.6.1.4.1
   and DS00 say [CTS].
5. The DS00 text says an embedded fault sets [FTONE_DET]; its figure and DS16
   say FCOMM_DET (p.31).
6. Wake tone duration: AN00 assumes about 1.6 ms. 90 pulses x 11-15 us is
   1.0-1.35 ms. AN00's rule is the conservative one.
7. Per-byte time: DS00's tWAIT formula uses 10 us/byte; the byte-to-byte
   figure is 10.875 us. The read-time estimate is given as a range for this
   reason.
8. Broadcast Read is used in DS16 and AN16 but forbidden with a BQ79600 base
   (DS00 p.15).
9. SCH-M wiring of the NFAULT pull-up: the design note says 100 k, and the
   sheet shows a 100 k (R2) plus four 10 k resistors on the SPI and NFAULT
   lines. The text extraction can't tell which resistor is which; check the
   schematic graphics.

Still UNVERIFIED: protector deglitch; stack-read behaviour past a break;
fault-tone per-hop total; open-wire comparison time and whether it uses the
filtered VCELL; VCELL accuracy while balancing; Enepaq sensor accuracy;
auto-address total time.
