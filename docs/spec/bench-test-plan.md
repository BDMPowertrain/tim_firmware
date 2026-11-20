# Bench test plan

The numbered **Bench cases** that prove on a real TIM what host tests cannot:
drivers, tasks, interrupts, timing on the wires. Written from the resolution
on [#24](https://github.com/BDMPowertrain/tim_firmware/issues/24) and the
cases later tickets added to it (#18, #23, #26, #69), for
[#70](https://github.com/BDMPowertrain/tim_firmware/issues/70). Words in
**bold** are `GLOSSARY.md` terms.

A Bench case has a stimulus, a start edge, an end edge and a pass criterion.
A **Bench measurement** has no pass criterion: it confirms an assumption and
feeds a number back into the spec, and is listed apart so an unconfirmed
assumption never reads as a passed test.

## Rules

1. **IDs are stable.** `COM-nn` for cases both images share, `BMS-nn` and
   `VCU-nn` for one image, `CAR-nn` for proofs that need the car, `M-nn` for
   Bench measurements. An ID is never reused. A case's script is
   `tests/bench/<ID>_<name>.py`.
2. **Faults come from outside the board** (#10): a switch, the CAN
   interface, the supply, or the debugger. The image carries no
   fault-injection code (spec #36, story 66); the shell's `hang` and `crash`
   are the only bench commands, and they are in every build.
3. **Pass:** the worst of 20 runs is at or under the **budget**, not the
   outer bound, with no extra margin, and every run's other checks held. A
   case with no time budget passes when all 20 runs pass. Fewer than 20 runs
   is INCOMPLETE, never a pass. A trip that comes too early (before its
   debounce) is proven by host tests only.
4. **Results:** each run of a case writes one file into
   `tests/bench/results/`, committed: case ID, image git commit, board
   serial, date, all 20 times, the worst, PASS, FAIL or INCOMPLETE, and a
   scope screenshot for a scope-timed case (`--scope-screenshot`). The Rig A
   scripts write it as `<ID>_<image>_<UTC time>.json`.
5. **When:** the full plan for an image passes on the exact commit that goes
   on the car. After a change, the cases it touches are re-run before merge,
   by the author's judgement. Nothing here runs in CI.
6. **Hang and crash cases** have no outside stimulus, so they are timed from
   the **last status frame on the bus**. This overstates the time by up to
   one period (20 ms BMS, 10 ms VCU).
7. **VCU timing without CAN decode:** pedal cases are timed in two parts. The
   scope times sensor line to fault-active test point; a script checks that
   the first 0x0C0 after the test-point edge carries zero torque. The budget
   applies to the scope time plus one 10 ms frame period.
8. **Late and skipped Control bus frames** (chosen here): a frame is late if
   it comes more than 1.5 periods after the one before it with its ID
   (judged for the frames sent every step or every second step: BMS_Status
   and BMS_Power at 20 ms, 0x0C0 at 10 ms, VCU_Status at 20 ms). A frame is
   skipped if its counter (E2E Profile 11, 0 to 14; 0x0C0's rolling
   counter, 0 to 15) does not step by one, or if `bus` counts `tx_skipped`.

Budgets marked *new* were set on #24; *amended* ones changed after it, with
the ticket that changed them. The rest come from the tickets named.

## Rigs

- **Rig A:** a bare TIM, a CAN interface on both buses (a ValueCAN: Control
  bus classic 500 kbit/s, DAQ bus CAN FD 500 kbit/s and 2 Mbit/s), an ST-Link,
  a two-channel scope, a switch from NRST to ground, a switch across the DAQ
  bus's CANH and CANL, a bench supply with a switch, and a supply that can be
  lowered to 2.5 V. Runs today.
- **Rig B:** a TIM with the BMS image, the BMS master, at least one slave and
  a cell source. The baseline cell source is a resistor ladder fed from a
  bench supply (one slave, 16 taps) with a switch that steps one tap across a
  threshold; temperature inputs come from a divider that a switch can step,
  open or short. Either way the start edge is the step on the tap, so the
  BQ79616 input filter is inside the measured time. The full 128-cell chain
  is needed only for the sweep-time measurement and chain comms loss at full
  length. Charging cases add `tools/charger_sim.py` playing the charger
  until the real one is on the bench.
- **Rig C:** a TIM with the VCU image and the VCU daughterboard, or pots on
  the mezzanine until it exists.

### Setting up Rig A

1. `pip install -r tests/bench/requirements.txt` (cantools and python-can
   from `requirements.txt`, pyserial, python-ics for the ValueCAN).
2. Terminate each bus once at the interface and once at the TIM, from a
   stored parameter so it survives the resets: `param set can1_term 1`,
   `param set can2_term 1`, `param save`, then reset. (`term` is a bench
   override and is lost at a reset.)
3. The shell is USART1 at 115200 baud. It needs its own USB serial adapter
   (`--shell COMx`); the ST-Link's virtual COM port is not wired to it on
   the bench TIM. Without `--shell`, a script asks the person to type each
   command and paste the reply.
4. Every script takes `--board` (the serial on the board's label) and
   `--commit` (the commit the flashed image was built from), and
   `--runs` (20 unless trying a script out). `--interface`,
   `--control-channel` and `--daq-channel` default to a ValueCAN with the
   Control bus on network 1 and the DAQ bus on network 2.
5. **The debug enable bit.** Any OpenOCD or GDB connection sets the core's
   debug enable bit until a power cycle, and the BMS takes every reset under
   it as power-on and never holds (ADR 0011). Power-cycle the board after a
   debugger session and before COM-01, COM-02, COM-06, COM-07 and COM-08 on
   the BMS. The scripts that use the debugger say so at the end.
6. On Rig A the BMS has no chain: it leaves STARTUP for FAULT after 2 s and
   BMS_OK never rises. The VCU's pedal and brake sensors read a supply fault,
   so its pedal Torque cuts are active, and it sits in GLV_ON.

## Cases both images share (Rig A)

Every COM case runs on both images; a script finds which from the status
frame on the bus (0x080 BMS, 0x090 VCU).

| ID | Case | Stimulus | Start edge | End edge | Budget | Pass criterion | Recorded |
|---|---|---|---|---|---|---|---|
| COM-01 | Hang command | `hang confirm` | Last status frame before the hang | VCU: first 0x0C0 after the reset. BMS: NRST low (scope); BMS_OK itself is BMS-21 | BMS 100 ms (expected at most about 85 ms measured this way). VCU 99 ms *new* (the inverter's own timeout) | Worst of 20 at or under the budget. VCU: no 0x0C0 after the command carries Enable=1, the first after the reset carries Enable=0 | 20 times; VCU from the bus, BMS from the scope |
| COM-02 | Crash command | `crash confirm` | As COM-01 | As COM-01 (pins floating at the reset) | As COM-01 | As COM-01, and `lastreset` gives a software reset with the Crash record "crash command in shellTask" and a Breadcrumb | 20 times; `lastreset` per run |
| COM-03 | Three Overruns in a second | GDB moves the step's clock on by one period at the end of three steps running (below) | The third Overrun | The fault bit in the status frame | Pass or fail | `step` counts 3 Overruns. BMS: an ordinary BMS fault (InternalActive), cleared after 1 s clean. VCU: TimingFault Torque cut; its recovery on pedal release is VCU-16 | How long the BMS fault was set; the Overrun count |
| COM-04 | DAQ bus shorted *new* | A switch shorts the DAQ bus's CANH to CANL for 60 s | Switch closed | Switch opened | No late or skipped frame (status gap 30 ms) | No control-step Overrun and no Control bus frame skipped or late over the 60 s | Worst status frame gap per run; Overrun and `tx_skipped` counts |
| COM-05 | DAQ send queue flooded *new* | The CAN interface holds the DAQ bus with 64-byte FD frames at ID 0x001 for 60 s | Flood started | Flood stopped | As COM-04 | As COM-04, and the TIM's own DAQ frame rate fell by at least half, or the run did not flood the queue and fails | As COM-04, and the TIM's DAQ frames a second before and during |
| COM-06 | Watchdog reset | `hang confirm` | The reset | The status frame 3 s after the first one | Pass or fail | BMS ends in FAULT_HELD, VCU in GLV_ON; both report LastResetNotPowerOn. BMS_OK never goes high during the reset (scope on MCU_BMS_OK, normal trigger on a rise) | Last status frame to first after it, per run; end state |
| COM-07 | Software reset | `crash confirm` | As COM-06 | As COM-06 | Pass or fail | As COM-06 | As COM-06 |
| COM-08 | NRST pin reset | A switch from NRST to ground | As COM-06 | As COM-06 | Pass or fail | As COM-06 | As COM-06 |
| COM-09 | Power cycle | The supply's switch off, then on | As COM-06 | As COM-06 | Pass or fail | BMS starts normally (not FAULT_HELD), VCU in GLV_ON; neither reports LastResetNotPowerOn. BMS_OK never high | As COM-06 |
| COM-10 | Brown-out reset | The supply lowered below 2.8 V (to 2.5 V) and back, on a provisioned board | As COM-06 | As COM-06 | Pass or fail | As COM-09 ("BOR flag set" is a Power-on reset, ADR 0011) | As COM-06; the thresholds are M-16 |
| COM-11 | Step timing while bank 2 is erased (#18 1) | `param save`, then 70 Events from the bus in 0.7 s so an Event log page is erased back to back with the writes | `param save` sent | 3 s after the last Event | No late or skipped frame (status gap 30 ms) | An Event log page was opened, the save succeeded, no Overrun, no frame late or skipped: proves the bank split (ADR 0014) | Worst status frame gap per run; pages opened |
| COM-12 | Power removed during a write (#18 2) | The supply cut during a `param save`, or while Events are written; alternate runs | The cut | The status frame 3 s after the start | Pass or fail | The board starts; the previous Parameter set (or the new one, if the save finished) is in force, never anything else; a torn Event log page is repaired with a storage repaired Event; the BMS does not end in FAULT_HELD. At least one run must tear a write, or the case proved nothing and fails | Per run: copy states, value in force, repair Events, torn or not |
| COM-13 | A normal flash leaves bank 2 untouched (#18 3) | OpenOCD `program <elf> verify`, core halted throughout | Bank 2 dumped before | Bank 2 dumped after | Pass or fail | The two dumps are identical, and bank 2 held something | Bytes that differed |
| COM-14 | Both Parameter copies corrupted (#18 4) | OpenOCD flips one bit in each copy inside its CRC, then puts them back | Board started on the corrupted copies | The status frame 3 s later | Pass or fail | ParametersDefaulted set; the VCU reaches GLV_ON and sends no Enable=1; after the copies are put back, ParametersDefaulted clears. The start procedure's refusal is VCU-19 | Per run: state and flags, corrupted and restored |
| COM-15 | Event flood (#18 6) | 1000 Events asked for from the bus in 10 s | The first Event | 3 s after the last | Pass or fail | At least 64 written, no stretch over the guard (a burst of 64, then one a second), and an "N events dropped" Event | Per run: Events written; the closest two after the burst |

**Events from the bus** (COM-11, COM-12, COM-15). Each frame becomes one
Event through the real path: on the VCU, INV_FaultCodes with new fault
bits (an inverter fault Event each); on the BMS, CHG_Status with a changed
flag (a charger flags Event each). On the BMS this puts it in Charge mode
until a power cycle, which is harmless on Rig A.

**COM-03's stimulus** (chosen here). A debugger halt cannot cause an
Overrun, because the RTOS tick and the cycle counter stop with the core. So
GDB breaks in `step_sched_next()` (`App/common/logic/Src/step_timing.c`),
the one function that judges an Overrun, on the control step's period
(Housekeeping calls it too, on 100 ms), and adds one period to its `now_ms`
three steps running. To the image each of those steps ended one period
late: it counts the Overrun, posts its Event and skips a period, as a long
step would. Nothing in the image changes for it.

**COM-05 and #69.** With #69 the BMS queues up to 15 DAQ frames every
100 ms (BMS_DaqStatus, 8 module summaries, at most 6 cell-dump frames), close
to its 16-deep send queue, so COM-04 and COM-05 run on the BMS image as well
as the VCU image, against an image with #69.

**#18 case 5,** the start procedure refused after a flash until the Pedal
check passes, needs pedals and is VCU-18 on Rig C.

### Scripts

`tests/bench/COM-01_hang_command.py` to `COM-15_event_flood.py`, one per
case, with shared pieces in `benchlib.py` (judging, the result file, bank 2
layouts; host-tested by `tests/host/test_benchlib.py`), `rig.py` (the CAN
interface, the shell, OpenOCD and GDB, the person at the bench),
`hang_crash.py` and `daq_isolation.py`. They use python-can and the DBC
files through cantools, so a frame layout is never written twice (ADR
0003). For example:

    python tests/bench/COM-01_hang_command.py --board TIM-07 --commit 3c8cb03a --shell COM9
    python tests/bench/COM-03_three_overruns.py --board TIM-07 --commit 3c8cb03a \
        --shell COM9 --elf build/Debug/bms/tim.elf

COM-03, COM-11, COM-12, COM-13, COM-14 and COM-15 need the ST-Link;
COM-03 and COM-13 also need the ELF that was flashed. Scripts for Rig B and
Rig C cases are not written yet.

## BMS cases (Rig B)

Timed from the step on the cell source to BMS_OK low. Budgets from
[BMS fault reaction-time budget](https://github.com/BDMPowertrain/tim_firmware/issues/14).

| ID | Case | Stimulus | Start edge | End edge | Budget | Pass criterion | Recorded |
|---|---|---|---|---|---|---|---|
| BMS-01 | Over-voltage | One tap stepped above 4.195 V | The step | BMS_OK low | 100 ms | Worst of 20 at or under | 20 times, scope |
| BMS-02 | Under-voltage | One tap stepped below 2.505 V | The step | BMS_OK low | 250 ms | As BMS-01 | As BMS-01 |
| BMS-03 | Over-temperature | One divider stepped past 58 C | The step | BMS_OK low | 550 ms | As BMS-01 | As BMS-01 |
| BMS-04 | Under-temperature, Charge mode | One divider stepped below 2 C, charger heard | The step | BMS_OK low | 550 ms | As BMS-01 | As BMS-01 |
| BMS-05 | Under-temperature in DRIVE | As BMS-04, no charger | The step | 5 s later | No trip in 5 s *new* | BMS_OK stays high | Pass or fail |
| BMS-06 | Temperature sensor open | One divider opened | The opening | BMS_OK low | 150 ms | As BMS-01 | As BMS-01, and which comparator fired (BMS-17) |
| BMS-07 | Temperature sensor shorted | One divider shorted | The short | BMS_OK low | 150 ms | As BMS-01 | As BMS-06 |
| BMS-08 | Cell-monitor chain unplugged | The chain's connector pulled | The pull | BMS_OK low | 150 ms | As BMS-01 | As BMS-01 |
| BMS-09 | Open sense wire | One sense wire opened | The opening | BMS_OK low | 2.5 s | As BMS-01 | As BMS-01 |
| BMS-10 | Low-voltage limit (3.0 V) | One tap stepped below 3.0 V, above 2.505 V | The step | 5 s later | No trip in 5 s *new* | BMS_OK stays high and the Discharge limit falls | Pass or fail; the limit |
| BMS-11 | Recovery | A fault from BMS-01 to BMS-04 cleared | The condition cleared | BMS_OK high | Between 1.0 s and 1.1 s *new* | Every run inside the window | 20 times |
| BMS-12 | Power-up with a fault present | Power on with one tap out of range | Power on | 10 s later | No rise in 10 s *new* | BMS_OK never rises | Pass or fail |
| BMS-13 | Control bus shorted | The Control bus's CANH shorted to CANL | The short | 10 s later | No trip in 10 s *new* | BMS_OK stays high, the Sweep carries on | Pass or fail |
| BMS-14 | Over-voltage comparator | One tap stepped above 4.225 V | The step | NFAULT low (scoped on NFAULT itself) | 100 ms | As BMS-01; record whether the firmware's fault came first | As BMS-01 |
| BMS-15 | Under-voltage comparator | One tap stepped below 2.4 V | The step | NFAULT low | 250 ms | As BMS-14 | As BMS-14 |
| BMS-16 | Over-temperature comparator | One divider stepped past the comparator (about 61.7 C) | The step | NFAULT low | 550 ms | As BMS-14 | As BMS-14 |
| BMS-17 | Comparator on an open and a shorted temperature sensor | As BMS-06 and BMS-07 | The opening or short | NFAULT low, if it falls | 150 ms if it falls | Which comparator fired is recorded; closes the unverified fact in #14 | Which comparator, the time |
| BMS-18 | FB.BMS_OK forced to disagree | The feedback line forced through a switch and a series resistor | The switch | MCU_BMS_OK low, internal fault | 150 ms | As BMS-01 | As BMS-01, the Event (where = 6) |
| BMS-19 | FB.NFAULT forced to disagree with the fault register | As BMS-18 | The switch | MCU_BMS_OK low, internal fault | 150 ms | As BMS-01 | As BMS-18 (where = 7) |
| BMS-20 | Shutdown Circuit sense held energised while BMS_OK is low | The BMS relay defeated, BMS_OK forced low | BMS_OK low | Internal fault bit | 520 ms *new* (500 ms plus one Sweep) | As BMS-01 | As BMS-18 (where = 8) |
| BMS-21 | Hang and crash against BMS_OK | COM-01 and COM-02 with BMS_OK high | Last BMS_Status | BMS_OK low | 100 ms | As COM-01 | As COM-01 |
| BMS-22 | Reset cases with BMS_OK high | COM-06 to COM-10 with BMS_OK high before each | The reset | End state | Pass or fail | BMS_OK never goes high during the reset (scope); end states as COM-06 to COM-10 | As COM-06 |

If the drawn circuit does not let a feedback line be forced without
fighting an output, that case (BMS-18 to BMS-20) falls back to a host test
and this plan says so.

**Charging** (Rig B with the charger stand-in), from
[Charger interface and charge control](https://github.com/BDMPowertrain/tim_firmware/issues/22):

| ID | Case | Stimulus | Start edge | End edge | Budget | Pass criterion | Recorded |
|---|---|---|---|---|---|---|---|
| BMS-23 | Charger timeout keeps BMS_OK up | `charger_sim.py --silent-after 10` | The last status | Charge WAITING | 5 s | BMS_OK stays high, stop requests keep coming, the charge restarts about 3 s after the charger returns | Pass or fail; the times |
| BMS-24 | Charger ignores its request | `charger_sim.py --over 1.5` | The excess current | BMS_OK low (charger disobeys) | 5 s | As BMS-01 | As BMS-01 |
| BMS-25 | The charge ends at the target | A charge to the end on real cells | Highest cell at the Charge target | Charge complete | Pass or fail | Charge complete arrives (no hovering at the target), the rested-cells Event 60 s later | Pass or fail; the log |

**Balancing** (Rig B on real cells), the cases from
[Balancing strategy and the Shutdown Circuit interlock](https://github.com/BDMPowertrain/tim_firmware/issues/23).
The cell reading error while bleeding is M-07, and the thermal run is
CAR-02.

| ID | Case | Stimulus | Start edge | End edge | Budget | Pass criterion | Recorded |
|---|---|---|---|---|---|---|---|
| BMS-26 | Shutdown Circuit sense opened during balancing | The Charging Shutdown Circuit opened | The opening | Balancing current stops (scope across a balancing resistor) | 40 ms | As BMS-01 | As BMS-01 |
| BMS-27 | MCU hang during balancing | `hang confirm` (or a debugger halt) while cells bleed | The hang | Balancing current stops | 10 s (the chip's timer) | As BMS-01 | As BMS-01 |
| BMS-28 | MCU reset during balancing | `crash confirm` while cells bleed | The reset | Balancing current stops | Before the first Sweep after the reset (the boot-time stop) | Every run | 20 times |
| BMS-29 | Charger silent pauses balancing | The stand-in stopped, then restarted | The last charger status | PAUSED (CHARGER_SILENT), every FET off; then bleeding again | Pause within 5 s; resume 3 s after the charger returns | Every run | 20 pairs of times |

## VCU cases (Rig C)

| ID | Case | Stimulus | Start edge | End edge | Budget | Pass criterion | Recorded |
|---|---|---|---|---|---|---|---|
| VCU-01 | APPS disagree by more than 10 points | One pot moved | Sensor line | Test point, plus a frame | 90 ms | Worst of 20 at or under; the first 0x0C0 after the test point carries zero torque | 20 times |
| VCU-02 | APPS open, shorted low, shorted high (each sensor) | A switch on the sensor line | Sensor line | Test point, plus a frame | 90 ms | As VCU-01 | As VCU-01 |
| VCU-03 | Brake sensor open, shorted (each sensor) | As VCU-02 | Sensor line | Test point, plus a frame | 90 ms | As VCU-01 | As VCU-01 |
| VCU-04 | Sensor supply lost | The supply line opened | Supply line | Test point, plus a frame | 90 ms *new* | As VCU-01 | As VCU-01 |
| VCU-05 | Brake engaged with the pedal above 25% | Brake pressed with the pedal held | Brake line | Test point, plus a frame | 20 ms | As VCU-01 | As VCU-01 |
| VCU-06 | Pedal change to command | A pedal step | Pedal line | The 0x0C0 that changes | 20 ms | As VCU-01 | As VCU-01 |
| VCU-07 | A Torque cut recovers only below 5% | A cut, then the pedal released in steps | Script check | | Pass or fail | Torque returns only below 5% | Pass or fail |
| VCU-08 | BMS_Status, BMS_Power or inverter frames stopped | The script playing them stops one | The last frame | The zero-torque frame | 120 ms *new* (100 ms timeout plus two steps) | As VCU-01 | As VCU-01 |
| VCU-09 | BMS_Limits stopped | As VCU-08 | The last frame | The zero-torque frame | 520 ms *amended* (was 320 ms: the BMS_Limits timeout became 500 ms on #12, #43) | As VCU-01 | As VCU-01 |
| VCU-10 | Bad CRC or stuck counter on a BMS frame | The script sends one | The first bad frame | The zero-torque frame | As a stop (VCU-08, VCU-09) | As VCU-01 | As VCU-01 |
| VCU-11 | BMS fault reported, inverter fault reported | The script sets the fault | The frame | The Enable=0 frame | 20 ms *new* | As VCU-01 | As VCU-01 |
| VCU-12 | Shutdown Circuit input opened | A switch | The input | Test point | 20 ms *new* (the opto-isolator's delay not yet in it) | As VCU-01 | As VCU-01 |
| VCU-13 | Control bus shorted | A switch across CANH and CANL | The short | Test point | 20 ms *new* | As VCU-01 | As VCU-01 |
| VCU-14 | Bus-off: 10 ms restart, latch on the third, clears after 5 s clean | Shorts on the Control bus | The bus-off | The restart, the latch, the clear | Pass or fail | Each as stated | The times (CAN interface) |
| VCU-15 | Start procedure happy path and Ready to Drive sound | Brake, start button | The start button | Ready to Drive; the sound | Sound 1 to 3 s | Every run | Script and scope |
| VCU-16 | Three Overruns: the Torque cut recovers on pedal release | COM-03's stimulus, then the pedal released | Pedal below 5% | Torque again | Pass or fail | The cut holds until the pedal is released, then clears | Pass or fail |
| VCU-17 | Reset in READY_TO_DRIVE | The reset cases above | The reset | End state | Pass or fail | GLV_ON, Enable=0 | As COM-06 |
| VCU-18 | Start refused after a flash until the Pedal check passes (#18 5) | A flash, then the start procedure, then a Pedal check | The flash | A start accepted | Pass or fail | Refused before the check, accepted after | Pass or fail |
| VCU-19 | Start refused with defaulted Parameters | COM-14's corruption, then the start procedure | The start button | | Pass or fail | Refused, the reason reported | Pass or fail |
| VCU-20 | Power limiter: ramp through the ceiling (#21) | Scripts play the inverter speed frame, BMS_Power and BMS_Limits | | | Pass or fail | No violation of either rule window (`tools/power_rule_windows.py`) | The 0x0C0 trace |
| VCU-21 | Power limiter: BMS ceiling below the target (#21) | As VCU-20 | | | Pass or fail | As VCU-20 | As VCU-20 |
| VCU-22 | Power limiter: BMS_Power timeout (#21) | As VCU-20, BMS_Power stopped | | | Pass or fail | Torque cut; the trim restarts at its minimum | As VCU-20 |
| VCU-23 | Power limiter: BMS_Limits timeout (#21) | As VCU-20, BMS_Limits stopped | | | Pass or fail | Torque cut | As VCU-20 |

Start-procedure refusals (button already held, no brake, pedal pressed) are
host tests only.

## Proofs on the car (not bench)

| ID | Proof | Pass criterion |
|---|---|---|
| CAR-01 | The 80 kW run: BMS_Power logs from a full-throttle run, before competition (#21) | No violation of either rule window, judged by `tools/power_rule_windows.py` |
| CAR-02 | The balancing thermal run on the real pack, 4 channels on per board (#23) | The 95 C die pause and the cell pause (55 C, resume under 50 C, as built in #68; #23 said 45 C) hold |

## Bench measurements (no pass criterion)

| ID | Measurement | Rig | What is recorded, and where it goes |
|---|---|---|---|
| M-01 | Reset to the first control-step Kick, per start-up stage (#17) | A | Each stage's time; expected under about 50 ms each (ADR 0013) |
| M-02 | Power-on to BMS_OK high | B | The time |
| M-03 | Sweep time on the full 128-cell chain | B, full chain | Link time per Sweep, and the `sweep` phase's worst in `step` (#88) |
| M-04 | Control step worst-case duration and stack high-water marks | A, B, C | `step` and `tasks`, against 1 ms (VCU) and 3 ms (BMS) of CPU per step and 25% stack free |
| M-05 | The G4 stays in bus-off until restarted, and queue mode sends the lowest ID first | A | Bus capture (#74) |
| M-06 | DAQ bus edges at 2 Mbit/s and 5 Mbit/s | A | Scope captures |
| M-07 | Cell reading error while a balancing FET is on (#23, ADR 0020) | B | The same cell and its neighbours with the FET on and paused; under about 2 mV, the pause-to-measure can go (#98) |
| M-08 | The inverter frame byte layouts from a bus capture | Car or inverter bench | The capture; marks the DBC's UNVERIFIED frames |
| M-09 | Idle with the Data log running (#26) | A, C | Idle share; at least 50% expected |
| M-10 | Worst-case step time with the Data log running (#26) | A, C | Against the same with logging off: no worse expected |
| M-11 | M-09 and M-10 again with the card pulled mid-write and with a deliberately slow card (#26) | A, C | As M-09, M-10. If one fails, the retreat order is in `App/vcu/Src/tasks.c`: drop the DAQ bus recording, then lower the IMU rate |
| M-12 | BMS step time with the snapshot encode (#69) | B | Chain running, so every step takes a Sweep; a 580-byte snapshot Record is encoded every fifth step. Worst step time with and without snapshot steps (`step`), and the Overrun count |
| M-13 | BMS DAQ frame cadence (#69) | A (frames carry the "not read" values) | The DAQ bus logged 60 s: the interval between frames of each ID, 0x610-0x61F about 200 ms, 0x620-0x64F about 1 s; `tx_queue_dropped` and `tx_rejected` stay 0 on an FD bus with nothing failing |
| M-14 | BMS card pulled while logging (#69) | A, and B during a charge | Pull the card mid-write, refit it after 10 s: RecordsDropped in BMS_DaqStatus; the gap in bms_pack and bms_snapshot sequence numbers as `tools/data_log_decode.py` reports it; the logger remounting into a new file |
| M-15 | CAN1 termination settle time, `BUS_TERM_SETTLE_MS` (#78) | A | Worst case, into `bus_term.h` |
| M-16 | Brown-out thresholds, falling and rising (#73) | A, variable supply | Both thresholds against DS12288 VBOR4, into `tools/provision.tcl` |

A BMS snapshot Record is written only on a step that took a Sweep. On Rig A
(no chain) the card holds only the pack Records, so any check of the
snapshot content or its 10 Hz rate needs Rig B.

## Cases owed by spec #105 (not built yet)

Given IDs when the work lands, from
[Spec: second firmware revision](https://github.com/BDMPowertrain/tim_firmware/issues/105):

- **Lamp check** (VCU, daughterboard rig, worst of 20 power cycles): GLV
  rising at the VCU supply input to the output on, 800 ms or less; on-time
  2.5 to 5.5 s; no Lamp check after a watchdog reset; the outputs off after
  a check cut short. Reset release to expander up is a Bench measurement.
- **Pack current:** sign and gain at commissioning, against the charger and
  the Energy Meter; the zero window; each plausibility trip (sensor
  unplugged, VREF open, Vout shorted); VCU_Status bit 47 and the trim
  pinned while the current is invalid.
- **Cooling:** the inverter's 0x0A0, 0x0A2 and 0x0A6 broadcast rates; fan
  and pump switching; run-on timing; the forced pump; an unplugged NTC
  giving full cooling and CoolingInputFailed; an expander unplugged and
  re-plugged, and one browned out alone (outputs back within 100 ms).

## Bench issues and this plan

The open bench tickets hold the step-by-step checks for one piece of work;
this plan holds the cases and budgets they prove.

| Issue | What it checks | Cases and measurements here |
|---|---|---|
| #72 | A power cut during a Parameter save (ECC NMI path) | COM-12 |
| #73 | The brown-out reset near 2.8 V | COM-10, M-16 |
| #74 | Bus-off timing and no stale frame after a restart | VCU-14, M-05 |
| #75 | CAN with a second TIM (DAQ bus and Control bus) | COM-04, COM-05 |
| #78 | CAN1 termination settle time | M-15 |
| #88 | The cell-monitor link and the Sweep | BMS-08, M-03, M-04 |
| #90 | Comparators, the open-wire check and the self-checks | BMS-09, BMS-14 to BMS-20 |
| #95 | Charge control with the stand-in and the real charger | BMS-23 to BMS-25 |
| #98 | Balancing on real cells | BMS-26 to BMS-29, M-07 |

## Open

- **Who operates the switch:** a hand switch, or a USB relay board driven by
  the script with the scope read over USB. Not decided; this plan says "a
  switch" for every stimulus so it reads the same either way. The Rig A
  scripts ask the person for each switch and each scope reading. COM-12
  needs a cut inside about 45 ms of a save, which a hand rarely manages.
- **Specialised BMS test equipment** (a cell simulator) in place of the
  resistor ladder: to be decided by the hardware owner.
- **The opto-isolator's delay** is not in VCU-12's 20 ms; add it once the
  daughterboard part is chosen.
- **Chosen while writing this plan (#70), open to change:** the 1.5-period
  late rule (Rules, 8); COM-03's stimulus; timing the BMS hang and crash on
  Rig A to NRST; COM-05 counting as flooded only if the TIM's DAQ rate
  halves; COM-12 failing without a torn write.
