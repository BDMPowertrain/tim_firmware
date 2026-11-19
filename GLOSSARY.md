# TIM Firmware

Firmware for the TIM board, which runs as either the car's VCU or its BMS master controller depending on the daughterboard it carries.

## Language

**TIM**:
The base board (Telemetry & IMU Module) carrying the MCU, IMU, microSD and two isolated CAN buses. It is hardware only, never one of the images.
_Avoid_: VCU board, BMS board, base image

**Image**:
One flashable firmware binary. There are exactly two: the **VCU image** and the **BMS image**.
_Avoid_: Target (a CMake word only), build, variant, firmware (for a single binary)

**VCU image**:
The image that runs on a TIM in the vehicle-control role. It is also the image used to bring up a bare TIM with no daughterboard.
_Avoid_: Practice firmware, bring-up image

**BMS image**:
The image that runs on a TIM carrying the BMS master daughterboard.

**Common code**:
Source compiled into both images.
_Avoid_: Shared library, core, base

**Logic**:
Code that makes decisions from values it is handed and never touches hardware, the RTOS or the clock itself. State machines, plausibility checks, limiters and message encode/decode are logic; drivers, tasks and the shell are not.
_Avoid_: Business logic, application layer, pure code

**Host test**:
A test that runs **Logic** on a development PC, with no TIM involved.
_Avoid_: Unit test, PC test, offline test

**Bench test**:
A test of a real **Image** on a real TIM, with faults injected from outside the board and reaction times measured on the wires.
_Avoid_: HIL test, integration test, hardware test

**Bench case**:
One numbered **Bench test** with a stimulus, a start edge, an end edge and a pass criterion.
_Avoid_: Test case, scenario, test item

**Bench measurement**:
A one-time bench reading that confirms an assumption and has no pass criterion.
_Avoid_: Characterisation test, informational test

**Rig**:
The set of hardware a **Bench case** needs: Rig A is a bare TIM, Rig B adds the BMS master and a slave, Rig C adds the VCU daughterboard.
_Avoid_: Setup, fixture, test bench

**Control step**:
The one periodic step in which an **Image** reads its inputs, decides, and writes its safety outputs. The VCU's is the torque path every 10 ms; the BMS's is the **Sweep** and its checks every 20 ms. Nothing else in the image writes a safety output or sends on the **Control bus**.
_Avoid_: Main loop, safety task, control loop, torque task

**Control bus**:
The CAN bus that carries everything the car needs to drive or charge safely. In the car it joins the VCU, BMS, inverter, Energy Meter and insulation monitor; on the hand cart it joins the BMS and the charger. It is the TIM's CAN1.
_Avoid_: Critical bus, main bus, powertrain bus

**DAQ bus**:
The CAN bus for logging and display data. Nothing on the **Control bus** waits on it and no safety function reads it. It is the TIM's CAN2.
_Avoid_: Telemetry bus, secondary bus

**Message database**:
The single definition of every CAN frame on both buses: its ID, sender, period and signal layout. The images and the bench tests both take their frame layouts from it.
_Avoid_: CAN matrix, signal list, DBC (the file format, not the concept)

**Mailbox**:
The place an image keeps the latest copy of one received CAN frame, together with how old it is and whether it can be trusted. A newer frame replaces the older one; nothing queues.
_Avoid_: RX buffer, cache, queue, signal store

**Substitute**:
The safe value a reader is given in place of a frame's contents whenever its **Mailbox** is not valid.
_Avoid_: Default, fallback value, last known value

**Ready to Drive**:
The VCU state in which the pedal can produce motor torque. The driver enters it deliberately, with the brake held and a press of the start button, and the car announces it with the Ready to Drive sound.
_Avoid_: RTD mode, drive mode, armed

**Torque cut**:
The VCU commanding zero torque while staying in **Ready to Drive**. It ends by itself once its cause is gone and the pedal has been released; the driver does not repeat the start procedure.
_Avoid_: Soft fault, inhibit, torque lockout

**Leaving Ready to Drive**:
The VCU dropping to a lower state with the inverter disabled. Torque returns only after the driver repeats the start procedure.
_Avoid_: Hard fault, shutdown (that is the Shutdown Circuit's word), trip

**Power limiter**:
The part of the VCU's torque path that holds the pack's discharge power at or under the **Power ceiling** by lowering the driver's torque request. It only ever reduces torque and is not a **Torque cut**: the driver keeps whatever torque fits under the ceiling.
_Avoid_: 80 kW limiter (that is one of its two ceilings), power cap, torque limiter

**Power ceiling**:
The discharge power the **Power limiter** holds the car to at this moment: the lower of the fixed target under the 80 kW rule limit and the power the BMS's **Discharge limit** allows.
_Avoid_: Power limit (that is the rule's 80 kW), setpoint, cap

**Discharge limit**:
The most pack current the BMS will accept being drawn right now. It is how the BMS asks for less power; the VCU obeys it and never looks at a cell value itself.
_Avoid_: DCL, current cap, BMS limit

**Derating**:
Lowering the **Discharge limit** or **Charge limit** gradually as a cell value nears a **BMS fault** threshold, so the car slows, or the charge eases off, before it stops. The BMS does it for cell voltage and cell temperature; the inverter does its own for motor and inverter temperature.
_Avoid_: Taper (the shape, not the act), limp mode, foldback

**Brake engaged**:
Brake pressure above the one threshold the VCU uses everywhere it needs to know the driver is braking: the start procedure, the brake-and-pedal check, and the brake light.
_Avoid_: Brake pressed, braking, hard braking

**Pedal travel**:
How far the accelerator pedal is pressed, from 0% (released) to 100% (fully pressed), as computed separately from each of its two sensors.
_Avoid_: Throttle, APPS value, pedal position

**TS Active**:
The VCU state in which the Tractive System carries voltage but the pedal produces no torque. It lasts from the end of precharge until the bus has discharged, so it covers a car that is powering down as well as one waiting for the driver.
_Avoid_: HV on, precharged, armed

**Start procedure**:
What the driver does to enter **Ready to Drive**: hold the brake, keep the pedal released, then press the start button.
_Avoid_: RTD sequence, arming

**Sweep**:
One complete reading of every cell voltage and every cell temperature in the pack, taken by the BMS as a single exchange with the cell monitors.
_Avoid_: Scan, poll, sample, measurement cycle

**Open-wire check**:
The cell monitors' test that every voltage sense wire still reaches its cell: a small current is drawn from each sense pin and any cell that then reads under a threshold has lost its wire, or the fuse in it. The BMS runs it once a second, after a **Sweep** and never in its place; two failures in a row are a **BMS fault**.
_Avoid_: Wire test, OW diagnostic, sense check

**BMS fault**:
A condition for which the BMS takes BMS_OK low. It ends by itself once the condition has stayed away long enough; keeping the Tractive System off until a person resets it is the shutdown board's job, not the BMS's.
_Avoid_: Trip, error, alarm, latched fault

**Low-voltage limit**:
The cell voltage below which the BMS starts **Derating** to protect cell life: the voltage taper's 100% point, a BMS parameter (3.0 V unset), judged on the sag-compensated minimum cell voltage. It is not a **BMS fault** and never takes BMS_OK low.
_Avoid_: Soft under-voltage, UV warning, cutoff

**Charge mode**:
The BMS operating with the pack on the hand cart and the charger connected. The BMS knows it is in charge mode because it hears the charger on the **Control bus**.
_Avoid_: Charging state, charger present

**Charge limit**:
The most pack current the BMS will accept being put into the pack right now. In **Charge mode** it is what the BMS asks the charger for; in the car it is zero, because the car never regenerates.
_Avoid_: CCL, charge current setpoint, charger request

**Charge target**:
The cell voltage at which a charge is finished. The BMS lowers the **Charge limit** as the highest cell nears it; it sits below the over-voltage **BMS fault** threshold and is not a fault itself.
_Avoid_: Full voltage, cutoff voltage, charge cutoff

**Charge complete**:
The BMS condition after a charge has reached the **Charge target** and the BMS has told the charger to stop. It lasts until a power cycle; the BMS does not start another charge by itself.
_Avoid_: Full, charged, end of charge (that is the moment, not the condition)

**Charging Shutdown Circuit**:
The hand cart's counterpart of the Shutdown Circuit: the Charger Shutdown Button, BMS_OK and the insulation monitor in series, holding the charging isolation relays closed. It is separate hardware from the car's Shutdown Circuit and only one of the two is ever connected to the pack.
_Avoid_: Charge SDC, charger interlock

**Balancing**:
The BMS drawing a small current from the higher cells so that every cell reaches the **Charge target** together. It happens only in **Charge mode**, and never while the Shutdown Circuit or the **Charging Shutdown Circuit** is open.
_Avoid_: Equalising, bleeding (that is what one cell does), top-up (that is a second charge)

**Balancing gate**:
The cell voltage, a fixed distance under the **Charge target**, that the highest cell must reach once before **Balancing** is allowed in a charge. Below it, voltage differences between cells do not yet show which cell holds more charge.
_Avoid_: Start balancing voltage, balance threshold

**Settled Sweep**:
A **Sweep** taken with every balancing FET off for at least the balancing pause. While **Balancing** bleeds, only the settled Sweep of each second chooses the cells to bleed and feeds the **Charge limit** taper and the end of the charge; every Sweep is still judged for the BMS faults. With no cell bleeding, every Sweep is settled.
_Avoid_: Paused sweep, clean sweep, rested reading (that is the reading 60 s after Charge complete)

**Cell spread**:
The highest cell voltage minus the lowest, taken from one **Sweep** with no cell being bled. It is what **Balancing** reduces and how the BMS knows balancing is finished.
_Avoid_: Delta, imbalance, mismatch

**Power-on reset**:
A reset caused by the board's supply rising through the MCU's brown-out threshold, whether GLV was switched on or the supply only sagged and recovered. The MCU cannot tell the two apart, so neither do the images.
_Avoid_: Cold boot, power cycle (that is the human act), brown-out (as a separate case)

**Held fault**:
The BMS keeping BMS_OK low until the next **Power-on reset**, because it came out of a reset that was not one. It is the state FAULT_HELD and the only **BMS fault** that does not end by itself.
_Avoid_: Latched fault (the latch is the shutdown board's), lockout

**Provisioning**:
The once-per-board setup of a TIM's MCU option settings, done before any **Image** is flashed.
_Avoid_: First-time setup, board init, commissioning (that is the inverter's)

**Lamp check**:
An indicator being held on for a few seconds after GLV comes on so that a person can see it works.
_Avoid_: Bulb check, LED test, self-test

**Kick**:
The **Control step** telling the MCU's watchdog that it is alive and finished its last step properly. If the kicks stop, the watchdog resets the MCU.
_Avoid_: Feed, pet, refresh, service the watchdog

**Overrun**:
A **Control step** that had not finished when its next period began. The late step is not caught up; the next one starts on the following period boundary.
_Avoid_: Deadline miss, late step, timing violation

**Housekeeping**:
The one Low-priority task in each **Image**, every 100 ms: it drives the heartbeat LED, takes the image's slow readings and gathers task statistics once a second. Being the lowest task, it is the first to starve, which is what **Background stalled** watches for.
_Avoid_: Heartbeat task, background task, idle work

**Background stalled**:
The warning the **Control step** raises when **Housekeeping** has not run for 1 s. It is reported on the **DAQ bus** and resets nothing; it is not a fault.
_Avoid_: Starvation fault, watchdog, hang

**Breadcrumb**:
The running note of where the **Control step** last was, kept where it survives a reset, so that after a watchdog reset the image can say what it was doing.
_Avoid_: Trace, checkpoint, last-gasp record

**Crash record**:
What an image saves about a failure it detected itself (the cause, the task, the fault address) just before it resets on purpose.
_Avoid_: Core dump, fault frame, crash log (the event log is a different thing)

**Event log**:
The small record each image keeps on the TIM itself of what happened and when, kept across resets and power-off and never dependent on the SD card.
_Avoid_: Fault log (most of what it holds is not a fault), crash log, black box

**Event**:
One entry in the **Event log**: a fault trip, a state change, a torque cut starting or ending, a reset and its cause, an **Overrun**.
_Avoid_: Log line, record, fault entry

**Data log**:
The running record each image writes to the SD card of what its **Control step** saw and decided, step by step, for study after a run. It is a convenience: a missing card loses it and nothing depends on it. The **Event log** is the authority on what happened.
_Avoid_: SD log, telemetry, trace, black box

**Record**:
One entry in the **Data log**: one **Control step**'s inputs and decisions, an IMU sample, a DAQ bus frame, or a copy of an **Event**.
_Avoid_: Log line, sample, row

**Parameter**:
A stored value a person sets at the car without rebuilding the image: a calibration (pedal and brake sensor end points) or a configuration (CAN termination, maximum torque). Protection thresholds and timings are not parameters; they are fixed in the image.
_Avoid_: Setting, config value, tunable

**Calibrated**:
The state of a VCU whose stored pedal and brake sensor end points are all present, pass their sanity checks and were captured for the way this image reads the sensors. A VCU that is not calibrated commands no torque and refuses the **Start procedure**.

**Pedal check**:
The driver pressing the accelerator fully and releasing it, and applying the brake, with the Tractive System off, so the VCU can confirm its stored calibration against the real pedals. Required once after a new image is flashed or a new calibration is saved, before the **Start procedure** is accepted.
_Avoid_: Pedal test, calibration (which captures new end points; the pedal check only confirms them)
