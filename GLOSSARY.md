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

**BMS fault**:
A condition for which the BMS takes BMS_OK low. It ends by itself once the condition has stayed away long enough; keeping the Tractive System off until a person resets it is the shutdown board's job, not the BMS's.
_Avoid_: Trip, error, alarm, latched fault

**Low-voltage limit**:
The cell voltage below which the BMS asks for less discharge power to protect cell life. It is not a **BMS fault** and never takes BMS_OK low.
_Avoid_: Soft under-voltage, UV warning, cutoff

**Charge mode**:
The BMS operating with the pack on the hand cart and the charger connected. The BMS knows it is in charge mode because it hears the charger on the **Control bus**.
_Avoid_: Charging state, charger present

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

**Parameter**:
A stored value a person sets at the car without rebuilding the image: a calibration (pedal and brake sensor end points) or a configuration (CAN termination, maximum torque). Protection thresholds and timings are not parameters; they are fixed in the image.
_Avoid_: Setting, config value, tunable

**Calibrated**:
The state of a VCU whose stored pedal and brake sensor end points are all present, pass their sanity checks and were captured for the way this image reads the sensors. A VCU that is not calibrated commands no torque and refuses the **Start procedure**.

**Pedal check**:
The driver pressing the accelerator fully and releasing it, and applying the brake, with the Tractive System off, so the VCU can confirm its stored calibration against the real pedals. Required once after a new image is flashed or a new calibration is saved, before the **Start procedure** is accepted.
_Avoid_: Pedal test, calibration (which captures new end points; the pedal check only confirms them)
