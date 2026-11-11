# Rules requirements census (FSAE Rules 2027)

Answers issue #2: which rules bind the VCU firmware, the BMS firmware, the CAN
network, and charging, with every number the rules give and every place they
demand detection without a reaction time.

## Source

- **Formula SAE Rules 2027**, file `Datasheets/FSAE_Rules_2027_V1-2.pdf`
  (untracked, main checkout only). Every page footer reads
  "Version 1.0 1 Sept 2026", 147 pages. The filename says "V1-2" but nothing
  inside the PDF says 1.2; see [Open points](#open-points).
- Page numbers below are the PDF's own "Page N of 147" footers. Quoted text is
  verbatim apart from line wrapping.
- GR.4.2.3 (p. 7): documents "Incorporated by Reference ... have the same
  validity as the Formula SAE Rules". This brings in the **Energy Meter User
  Manual** (EV.3.2.2) and the **Rules FAQ** (EV.6.1.1, EV.8.6.2). Neither is in
  the PDF; anything that depends on them is marked **unverified**.

Section map: T.3 Brakes p. 65-66, T.4 Electronic Throttle Components p. 67-68,
T.9 Electrical Equipment p. 75-76, EV.3 Electrical Limitations p. 92-94, EV.4
Components p. 94-96, EV.5 Energy Storage p. 96-99, EV.6 Status Indicators
p. 99-101, EV.8 Shutdown System p. 105-110, EV.9 Charger p. 110-111, EV.10
Vehicle Operations p. 112, IN.5 / IN.10 / IN.13 / IN.14 Inspection p. 116-122,
D.12-D.14 Endurance / Efficiency p. 141-147.

**Note on T.3 numbering.** On p. 66 the rule numbers sit in a separate column
that text extraction does not align with the paragraphs. The T.3.2.x / T.3.3.x
/ T.3.4.x numbers below were reconstructed by matching the 16 labels to the 16
paragraphs in order (counts match exactly), but they are not machine-verified.
Check against the rendered page before quoting them in an ESF.

---

## 1. VCU (torque path, driver interface)

### Torque authority and direction

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.3.1.1 | 93 | "Supplying power to the motor to drive the vehicle in reverse is prohibited" | |
| EV.3.1.2 | 93 | "Drive by wire control of wheel torque is permitted" | |
| EV.3.1.3 | 93 | "Any algorithm or electronic control unit that can adjust the requested wheel torque may only decrease the total driver requested torque and must not increase it" | |
| EV.3.3.3 | 93 | "The powertrain must not regenerate energy when vehicle speed is between 0 and 5 km/hr" | 0-5 km/h |
| T.3.2.4 (reconstructed) | 66 | "(EV only) ... a. The first 90% of the Brake Pedal travel may be used to regenerate energy without actuating the hydraulic brake system b. The remaining Brake Pedal travel must directly operate the hydraulic brake system. Brake energy regeneration may stay active" | 90% |
| T.1.6.1.b | 58 | "Pedal Travel is the percent of travel from a fully released position to a fully applied position. 0% is fully released and 100% is fully applied." | 0%, 100% |
| T.3.1.6 | 65 | "\"Brake by Wire\" systems are prohibited" | |

### APPS (T.4.2, referenced from EV.4.5)

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| T.4.2.2 | 67 | "Two or more electrically separate sensors must be used as APPSs." | >= 2 sensors |
| T.4.2.3 | 67 | Different transfer functions; "a pull-up or pull-down resistor in circuit to bring an open circuit input to 0% pedal travel"; "At any pedal position above 10%, the pedal travel output of a single sensor must differ by minimum 10% from any other sensor" | 10%, 10% |
| T.4.2.4 | 67 | "Implausibility is defined as a deviation of more than 10% Pedal Travel between the sensors or other failure as defined in this Section T.4.2. Use of values larger than 10% Pedal Travel require justification in the ETC Systems Form and may not be approved" | 10% |
| T.4.2.5 | 67 | "If an Implausibility occurs between the values of the APPSs and persists for more than 100 msec, the power to the ... (EV) Motor(s) must be immediately stopped completely. (EV only) It is not necessary to Open the Shutdown Circuit, the motor controller(s) stopping the power to the Motor(s) is sufficient." | 100 ms |
| T.4.2.6 | 67 | With three sensors, "any two sensors that agree within 10% Pedal Travel may be used to define the ... (EV) torque target and the 3rd APPS may be ignored." | 10% |
| T.4.2.7 | 67 | Each APPS checkable at Tech by a detachable connector or "An inline switchable breakout box" | |
| T.4.2.8 | 67 | "The APPS signals must be sent directly to a controller using an analogue signal or via a digital data transmission bus such as CAN or FlexRay." | |
| T.4.2.9 | 67 | "Any failure of the APPS or APPS wiring must be detectable by the controller and must be treated like an Implausibility" | (inherits 100 ms of T.4.2.5) |
| T.4.2.10 | 67-68 | Analogue: failed on "open circuit or short circuit condition which generates a signal outside of the operating range, for example <0.5 V or >4.5 V"; pull-ups/downs must make open circuit detected | 0.5 V, 4.5 V (examples, not limits) |
| T.4.2.11 | 68 | Digital transmission: failures "must include but are not limited to the failure of the APPS, APPS signals being out of range, corruption of the message and loss of messages and the associated time outs" (11.a, the written description, is "IC Only") | timeout not given |
| T.4.2.12 | 68 | "the integrity of the torque command signal is important in all stages" | |

### BSE (T.4.3, referenced from EV.4.6)

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| T.4.3.1 | 68 | "a sensor or switch to measure brake pedal position or brake system pressure" | |
| T.4.3.2 | 68 | BSE checkable at Tech by detachable connector or breakout box "without affecting any other connections" | |
| T.4.3.3 | 68 | Analogue or CAN/FlexRay direct to a controller. "Any failure of the BSE or BSE wiring that persists more than 100 msec must be detectable by the controller and treated like an implausibility and power to the ... (EV) Motor(s) must be immediately stopped completely. (EV only) It is not necessary to completely deactivate the Tractive System" | 100 ms |
| T.4.3.4 | 68 | Analogue failure: open/short "outside of the operating range, for example <0.5 V or >4.5 V"; pull resistors required | 0.5 V, 4.5 V (examples) |
| T.4.3.5 | 68 | Digital: failure modes incl. "corruption of the message and loss of messages and the associated time outs"; "c. In all cases a sensor failure must immediately shutdown power to the motor(s)" | timeout not given |

### APPS / Brake Pedal Plausibility Check

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.4.7.1 | 95 | "Must monitor for the two conditions: The mechanical brakes are engaged EV.4.6, T.3.2.4 / The APPS signals more than 25% Pedal Travel" | 25% |
| EV.4.7.2 | 95 | "a. Power to the Motor(s) must be immediately and completely shut down b. The Motor shut down must stay active until the APPS signals less than 5% Pedal Travel, with or without brake operation. The team must be able to demonstrate these actions at Technical Inspection" | 5% reset; no persistence time |

"Mechanical brakes are engaged" has no numeric threshold anywhere in the rules.

### Ready to Drive

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.10.1 | 112 | "The driver must complete the Activation Sequence without external assistance after the Master Switches EV.8.9 are ON" | |
| EV.10.2 | 112 | Sequence: "a. Low Voltage (GLV) System b. Tractive System Active c. Ready to Drive" | |
| EV.10.4.1 | 112 | Tractive System Active: "High Voltage is present outside of the Tractive Battery Container" | |
| EV.10.4.2 | 112 | TS Active not possible until "GLV System is Energized" and "Shutdown Circuit is Closed" | |
| EV.10.6.1 | 112 | Ready to Drive: "the Motor(s) will respond to the input of the APPS" | |
| EV.10.6.2 | 112 | Not possible until all three at the same time: TS Active; "The Brake Pedal is pressed and held to engage the mechanical brakes T.3.2"; "The driver does a manual action to start Ready to Drive" (e.g. a cockpit button) | |
| EV.10.7.1 | 112 | "The vehicle must make a characteristic sound when it is Ready to Drive" | |
| EV.10.7.2 | 112 | "a. Sounded continuously for minimum 1 second and maximum 3 seconds b. A minimum sound level of 80 dBA, fast weighting IN.10.3 c. Easily recognizable..." | 1-3 s, 80 dBA |
| EV.10.7.3 | 112 | "The vehicle must not make other sounds similar to the Ready to Drive Sound." | |
| IN.10.3 | 119 | Measured "in a radius of 2 m around the vehicle" | 2 m |
| D.12.4.2 | 142 | After Driver Change, "maximum 120 seconds (two minutes) to ... (EV) enter Ready to Drive" | 120 s |
| D.12.8.1.b/f/h | 143 | Driver Change: first driver "turns off the ... Tractive System", second driver re-enables it | |

### Brake light

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| T.3.4.1 (reconstructed) | 66 | "The vehicle must have a Brake Light that is clearly visible from the rear in very bright sunlight." | |
| T.3.4.2 (reconstructed) | 66 | Red on black, min "shining surface of 15 cm2", mounted between wheel centreline and shoulder height, approx. on centreline | 15 cm² |
| T.3.4.3 / T.3.4.4 (reconstructed) | 66 | LEDs without diffuser "not more than 20 mm apart"; single line "minimum length is 150 mm" | 20 mm, 150 mm |

**The 2027 rules give no activation condition for the Brake Light.** Nothing
in T.3.4 says when it must illuminate (brake pressure, regen, threshold). Any
firmware trigger is a team choice, not a rule.

### Things the VCU must NOT implement (hard-wired / non-programmable by rule)

These bound what firmware may own:

| Rule | Page | Requirement |
|---|---|---|
| EV.8.7.1 | 109 | BSPD: "a standalone nonprogrammable circuit", "in addition to the APPS / Brake Pedal Plausibility Check (EV.4.7)" |
| EV.8.7.2 | 109 | BSPD opens SDC when "Demand for Hard Braking" and current giving "5 kW of electrical power in the DC circuit ... at the nominal battery voltage" coexist "for more than 0.5 sec" (5 kW, 0.5 s). "Hard Braking" has no numeric definition. |
| EV.8.7.3 | 109 | BSPD opens SDC on "an open or short circuit in any sensor input" |
| T.3.3.5 (reconstructed) | 66 | BOTS "must be implemented with analog components, not using programmable logic controllers, engine control units, or similar" |
| EV.8.6.4 | 108 | IMD monitoring "must be done without the influence of any programmable logic" |
| EV.5.7.1 | 98 | Voltage Indicator: "hard wired electronics with no software control" |
| EV.6.2.6.b | 101 | Ready to Move Light: "directly controlled by the voltage present in the Tractive System using hard wired electronics ... Software control is not permitted." |
| EV.8.9.1.b | 110 | Master Switches "direct acting, not act through a relay or logic" |

Firmware MAY drive: the Lamp Check ("may use software control (logical OR
function)", EV.6.3.1, p. 101), and the Tractive System Status Indicator (EV.6.1
contains no hard-wiring requirement).

---

## 2. BMS

### When and what it monitors

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.8.3.1 | 107 | "A Battery Management System must monitor the Tractive Battery Voltage EV.8.4 and Temperature EV.8.5 when the: a. Tractive System is Active b. Tractive Battery is connected to a Charger" | |
| EV.8.3.2 | 107 | "galvanic isolation at each Module to Module boundary, as approved in the ESF" | |
| EV.8.3.3 | 107 | "**Cell balancing is not permitted when the Shutdown Circuit is Open** ( EV.8.2, EV.9.4 )" | |
| EV.8.3.4 | 107 | "The BMS must monitor for: a. Voltage values outside the permitted range EV.8.4.2 b. Voltage sense Overcurrent Protection device(s) blown or tripped c. Temperature values outside the permitted range EV.8.5.2 d. Missing or interrupted voltage or temperature measurements e. A fault in the BMS" | no times |
| EV.8.3.5 | 107 | On any EV.8.3.4 condition: "a. Open the Shutdown Circuit EV.8.2.2 b. Turn on the BMS Indicator Light and the Tractive System Status Indicator ... The two lights must stay on until the BMS is manually reset EV.8.2.3" | no reaction time |
| EV.8.3.6 | 107 | BMS Indicator Light: red, visible to seated driver in sunlight, lettered "BMS" | |
| EV.8.3.7 | 107 | BMS Indicator Light must do a Lamp Check (EV.6.3) | |

### Cell voltage

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.8.4.1 | 107 | "The BMS must measure the voltage of each Cell. When single Cells are directly connected in parallel, only one voltage measurement is needed" | every series element |
| EV.8.4.2 | 107 | "Cell Voltage levels must stay between the permitted minimum and maximum cell voltage levels stated in the cell data sheet. Measurement accuracy must be considered." | from datasheet |
| EV.8.4.3 | 107 | Sense wires need Overcurrent Protection "in the conductor, wire or PCB trace which is directly connected to the cell tab", rated >= max Module voltage | |
| EV.5.2.4 | 97 | Soldering sense wires to cells for the BMS is permitted | |

### Cell temperature

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.8.5.1 | 107 | "The BMS must measure the temperatures of the Tractive Battery cells" | |
| EV.8.5.2 | 108 | "Temperatures (considering measurement accuracy) must stay below the lower of the two: The maximum cell temperature limit stated in the cell data sheet / 60°C" | **60 °C** cap |
| EV.8.5.3 | 108 | "Cell temperatures must be measured at the negative terminal of the respective cell" | |
| EV.8.5.4 | 108 | Sensor in direct contact with "The negative terminal itself" or "The negative terminal busbar less than 10 mm away from the spot weld or clamping source on the negative cell terminal" | 10 mm |
| EV.8.5.5 | 108 | Lithium: "a minimum of 20% of the cells must be monitored", "equally distributed"; "should" monitor each cell | 20% |
| EV.8.5.6 | 108 | One sensor may cover several cells "if EV.8.5 is met for all cells sensed" | |
| EV.8.5.7 | 108 | Sensors need electrical isolation (at the sensor or in the sensing circuit), considering TS/GLV and common-mode voltages | |

### Fuses and fusible links

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.8.3.4.b | 107 | Detect blown/tripped voltage-sense Overcurrent Protection | |
| EV.7.6.5 | 104 | Low-voltage-rated fusible links in cell connections are allowed only if all three hold, one being: "The BMS can detect an open fusible link and will Open the Shutdown Circuit EV.8.2.2 if a fault is detected" (series OCP <= one third of the sum of the parallel links) | 1/3 |
| EV.7.6.7 | 105 | Cell-internal OCP usually falls under EV.7.6.5, so the same BMS detection applies | |

### Shutdown Circuit behaviour the BMS participates in

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.8.1.1 | 105 | SDC = BMS, IMD, BSPD, Interlocks, GLVMS/TSMS, Shutdown Buttons, BOTS, Inertia Switch, in series | |
| EV.8.1.2 | 106 | SDC "must directly carry the current driving the Isolation Relays (IRs) and the Precharge Circuit Relay" | |
| EV.8.1.4 | 106 | "The BMS, IMD, and BSPD parts of the Shutdown Circuit must be Normally Open" | |
| EV.8.1.5 | 106 | BMS, IMD, BSPD need "completely independent circuits to Open the Shutdown Circuit"; no back-feed on failure | |
| EV.8.1.7 | 106 | Demonstrate all SDC features at Tech | |
| EV.8.2.1 | 106 | SDC must Open on "Operation of, or detection from any of the components" or "Any shutdown of the GLV System" | |
| EV.8.2.2 | 106 | On opening: TS shuts down; "All Tractive Battery current flow must stop immediately"; TS voltage Low Voltage "in five seconds or less"; motors spin free, no torque | 5 s |
| EV.8.2.3 | 106-107 | **Latch**: when BMS/IMD/BSPD open SDC, TS "must stay disabled until manually reset", reset "only by manual action of a person directly at the vehicle", "The driver must not be able to reactivate", Shutdown Buttons / TSMS "must not let the Shutdown Circuit Close" | |
| EV.5.4.5 | 98 | "A capacitor may be used to hold the Isolation Relays closed for maximum 250 ms after the Shutdown Circuit Opens" | 250 ms |
| T.9.4.3.c/d | 76 | Contrast: the Inertia Switch "Must latch until manually reset" but "May be reset by the driver" (8-11 g) | 8-11 g |

### Precharge (wherever it is controlled)

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.5.6.1 | 98 | Precharge must charge the Intermediate Circuit "to minimum 90% of the Tractive System voltage before closing the second Isolation Relay"; "supplied from the Shutdown Circuit" | 90% |
| EV.5.6.2.a | 98 | "The end of precharge must be controlled by feedback by monitoring the voltage in the Intermediate Circuit" (a timer alone does not comply) | |
| EV.5.6.3 | 98 | Discharge always active when SDC open, handles max TS voltage "for minimum 15 seconds" | 15 s |
| EV.5.6.5 | 98 | "The precharge relay must be a mechanical type relay" | |

### Tractive System Status Indicator and Lamp Check

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.6.1.7 | 100 | With GLVMS ON: no faults = green ON / red OFF; "Fault in one or the two: BMS EV.8.3.5 or IMD EV.8.6.5" = green OFF / red "Flash 2.5 Hz to 5 Hz, 50% duty cycle" | 2.5-5 Hz, 50% |
| EV.6.3.2 | 101 | Lamp Check: all indicators at once, "Start in 1 second or less after the GLVMS is turned ON", "ON continuously for 2 to 6 seconds" | <= 1 s, 2-6 s |
| EV.6.3.3 | 101 | Lamp Check logic "must not prevent the function of any indicator" | |

### Pack-level constraints the BMS design inherits

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.3.3.2 | 93 | Max voltage between any two points "must not exceed 600 V DC" | 600 V |
| EV.5.1.1 | 96 | Each Module: <= 120 V DC static, <= 6 MJ, <= 12 kg | 120 V, 6 MJ, 12 kg |
| EV.7.5.4 | 103 | The BMS is one of the few GLV systems allowed inside the Tractive Battery Container | |
| EV.7.5.9 | 104 | "All connections to external devices such as laptops from a Tractive System component must include galvanic isolation" | |
| T.9.1.1 / T.9.1.2 | 75 | HV: "more than 60 V DC or 25 V AC RMS"; LV: "less than and including 60 V DC or 25 V AC RMS" | 60 V DC, 25 V AC |

---

## 3. CAN network and Energy Meter

The rules contain **no requirements on the vehicle CAN bus itself** (no bit
rate, IDs, or mandated messages). CAN is touched only where a safety signal
travels over it.

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| T.4.2.8, T.4.3.3 | 67-68 | APPS and BSE may reach the controller "via a digital data transmission bus such as CAN or FlexRay" | |
| T.4.2.11.b, T.4.3.5.b | 68 | If so, failure modes must include "corruption of the message and loss of messages and the associated time outs" | timeout not given |
| T.4.3.5.c | 68 | "In all cases a sensor failure must immediately shutdown power to the motor(s)" | |
| EV.3.2.1 | 93 | "All Electric Vehicles must run with the Energy Meter provided at the event" | |
| EV.3.2.2 / EV.3.2.3 | 93 | Install per the Energy Meter User Manual (incorporated by reference, GR.4.2.3) | **unverified**: manual not available |
| EV.3.2.4 | 93 | "All Tractive System power must flow through the Energy Meter" | |
| EV.3.2.5 | 93 | Teams "must download and associate their Energy Meter data in a manner and timeframe specified by the organizer" | |
| EV.3.2.6 | 93 | "Power and Voltage and temperature limits will be checked by the Energy Meter data. Energy is calculated as the time integrated value of the measured voltage multiplied by the measured current logged by the Energy Meter" | |
| EV.3.3.1 | 93 | "The maximum power measured by the Energy Meter must not exceed 80 kW" | **80 kW** |
| EV.3.4.1 | 93 | Violation = over 80 kW or over 600 V "Continuously for 100 ms or more" or "After a moving average over 500 ms is applied" | 100 ms, 500 ms |
| EV.3.4.2 / EV.3.4.3 | 93 | Missing EM data may count as a Violation; tampering may DQ | |
| EV.3.5.1 / EV.3.5.2 | 94 | Accel/Skidpad/Autocross: each run with a Violation DQs the team's best run; Endurance: 60 s penalty per Violation | 60 s |
| EV.5.1.2 | 96 | No energy storage after the Energy Meter except "reasonably sized intermediate circuit capacitors" | |
| EV.5.5.2 | 98 | The Energy Meter must not be the MSD | |
| D.13.2.6 | 146 | Efficiency: EM data downloaded "to measure energy used and check for Violations" | |
| D.13.4.1 / D.13.4.2 | 146 | 0.65 kg CO2 per kWh; "Full credit is given for energy recovered through regenerative braking" | 0.65 kg/kWh |
| D.13.4.5 | 146 | EV efficiency floor "equivalent to 20.02 kg CO2/100km" | 20.02 kg/100 km |

Reading of EV.3.4.1: the rule names two tests joined by "for one or the two
conditions"; it does not state whether either alone triggers a Violation. The
conservative reading for a VCU power limiter is that both must be met (never
above 80 kW for 100 ms, and the 500 ms moving average never above 80 kW). The
EV.3.2.6 mention of "temperature limits" checked by the EM has no matching
temperature limit in EV.3; its meaning depends on the EM manual
(**unverified**).

---

## 4. Charging

| Rule | Page | Requirement | Numbers |
|---|---|---|---|
| EV.12.5.1 | 114 | "The Tractive Battery Pack must be removed from the vehicle and put onto the Hand Cart EV.4.10 for Charging" (so the BMS charges without the VCU present) | |
| EV.12.5.2-4 | 114 | Designated area only; a knowledgeable team member stays with the pack; label with team name and ESO phone | |
| EV.8.3.1.b | 107 | BMS monitors voltage and temperature while connected to a Charger | |
| EV.9.3.1 | 111 | Charging Shutdown Circuit = Charger Shutdown Button, BMS, IMD | |
| EV.9.3.2 | 111 | BMS and IMD parts "Normally Open", "completely independent circuits", no back-feed | |
| EV.9.4.1 | 111 | "When Charging, the BMS and IMD must: a. Monitor the Tractive Battery b. Open the Charging Shutdown Circuit if a fault is detected" | no time |
| EV.9.4.2 | 111 | On opening: "All current flow to the Tractive Battery must stop immediately"; TS Low Voltage "in five seconds or less"; "The Charger must be turned off"; "The Charger must stay disabled until manually reset" | 5 s |
| EV.8.3.3 | 107 | No cell balancing when the SDC is open (cites EV.9.4, so it covers the Charging Shutdown Circuit too) | |
| EV.9.2.4 | 111 | Charger connector live "only when correctly connected to the Tractive Battery Pack" | |
| EV.9.2.7 | 111 | Charger Shutdown Button: push-pull/push-rotate, >= 24 mm, opens the Charging Shutdown Circuit | 24 mm |
| EV.9.1.1 / EV.9.1.2 | 110-111 | Demonstrate everything at Tech (IN.5.1); chargers sealed after approval | |
| EV.5.8.2 | 99 | Two TSMPs in the Charger, available during charging | |

The rules contain **no charge-current, charge-voltage or charger-communication
requirement** beyond the shutdown behaviour above; cell limits come only from
EV.8.4.2 / EV.8.5.2.

---

## 5. Inspection items that exercise firmware

| Rule | Page | What is tested |
|---|---|---|
| EV.4.7.2 | 95 | Demonstrate APPS/brake plausibility shutdown and the < 5% reset |
| T.4.2.7, T.4.3.2 | 67-68 | Unplug each APPS / BSE signal at Tech; the controller must react |
| EV.8.1.7 | 106 | Demonstrate every SDC feature (BMS fault opening and latching included) |
| EV.9.1.1, IN.5 | 110, 116-117 | Charger and Charging Shutdown Circuit; pack may be "marked or sealed" and "Additional monitoring may be installed" (IN.5.2.2-3) |
| IN.10.1 | 119 | IMD shuts the TS down "in 30 seconds or less" at a resistance <= 50% of the response value (>= 500 Ohm/V, EV.8.6.3) |
| IN.10.3 | 119 | Ready to Drive Sound level |
| IN.13.1 | 121 | Rain test: TS Active but "must not be in Ready to Drive mode", 120 s spray + 120 s observe, IMD must not trip during 240 s |
| IN.14.3 | 122 | Brake test: mechanical brakes only "with no aid from the powertrain"; driver switches off the TS (cockpit button) before braking |
| IN.16.2.3 | 123 | After Inspection Approval, "any software calibration changes" are permitted modifications |

---

## Every numeric value the rules specify (firmware-relevant)

| Value | Rule | Meaning |
|---|---|---|
| 80 kW | EV.3.3.1 | Max power measured by the Energy Meter |
| 600 V DC | EV.3.3.2 | Max voltage between any two points |
| 100 ms | EV.3.4.1 | Power/voltage Violation if exceeded continuously this long |
| 500 ms | EV.3.4.1 | Moving-average window for Violation |
| 60 s | EV.3.5.2 | Endurance penalty per Violation |
| 0-5 km/h | EV.3.3.3 | No regeneration in this speed band |
| 90% | T.3.2.4 | Brake pedal travel usable for regen before hydraulics |
| 10% | T.4.2.3, T.4.2.4, T.4.2.6 | Min sensor offset above 10% travel; implausibility deviation; 3-sensor agreement |
| 100 ms | T.4.2.5, T.4.3.3 | APPS implausibility / BSE failure persistence before shutdown |
| 0.5 V / 4.5 V | T.4.2.10, T.4.3.4 | Example out-of-range thresholds (illustrative, "for example") |
| 25% | EV.4.7.1 | APPS travel that, with brakes engaged, triggers plausibility shutdown |
| 5% | EV.4.7.2 | APPS travel below which plausibility shutdown clears |
| 1-3 s | EV.10.7.2 | Ready to Drive Sound duration |
| 80 dBA, 2 m | EV.10.7.2, IN.10.3 | RTD sound level, fast weighting, measured within 2 m |
| 120 s | D.12.4.2 | Time to re-enter Ready to Drive after Driver Change |
| 5 kW, 0.5 s | EV.8.7.2 | BSPD trip (hardware, but the VCU should not cause nuisance trips) |
| 60 °C | EV.8.5.2 | Cell temperature ceiling (or datasheet max if lower), accuracy included |
| 10 mm | EV.8.5.4 | Max sensor distance from negative-terminal weld/clamp on busbar |
| 20% | EV.8.5.5 | Min share of lithium cells temperature-monitored |
| datasheet min/max | EV.8.4.2 | Cell voltage limits, accuracy included |
| 1/3 | EV.7.6.5 | Series OCP rating vs. sum of parallel fusible links |
| 5 s | EV.8.2.2.c, EV.9.4.2.b | TS voltage to Low Voltage after SDC / Charging SDC opens |
| 250 ms | EV.5.4.5 | Max IR hold-up by capacitor after SDC opens |
| 90% | EV.5.6.1 | Min precharge level before closing second IR |
| 15 s | EV.5.6.3.d | Discharge circuit rating at max TS voltage |
| 60 V / 50 V | EV.5.7.3, EV.6.2.7 | Voltage Indicator / RTML on at >= 60 V, off at < 50 V (hard-wired) |
| 60 V DC, 25 V AC RMS | T.9.1.1-2 | HV / LV boundary |
| 2.5-5 Hz, 50% | EV.6.1.7.c | TS Status Indicator red flash on BMS/IMD fault |
| 0.66-1.33 Hz, 50% | EV.6.2.7.b | Ready to Move Light flash (hard-wired) |
| <= 1 s, 2-6 s | EV.6.3.2 | Lamp Check start after GLVMS ON; Lamp Check duration |
| 120 V, 6 MJ, 12 kg | EV.5.1.1 | Module limits |
| 500 Ohm/V | EV.8.6.3, IN.10.2.2 | IMD response value / insulation minimum |
| 30 s, 50% | IN.10.1.2 | IMD test shutdown time and fault resistance |
| 240 s | IN.13.3.1 | Rain test duration (TS Active, not Ready to Drive) |
| 8-11 g | T.9.4.3.a | Inertia Switch trigger |
| 0.65 kg CO2/kWh; 20.02 kg CO2/100 km | D.13.4.1, D.13.4.5 | Efficiency conversion and EV floor |

## Detection or reaction demanded with no time given

1. **EV.4.7.2.a** (p. 95): APPS > 25% with brakes engaged, power "immediately and completely shut down". No persistence or reaction time, and no threshold for "brakes engaged".
2. **T.4.2.5 / T.4.3.3 / T.4.3.5.c** (p. 67-68): after the 100 ms persistence, power stopped "immediately". The reaction itself has no time.
3. **T.4.2.9** (p. 67): any APPS or wiring failure "must be detectable"; no detection time beyond inheriting T.4.2.5.
4. **T.4.2.11.b / T.4.3.5.b** (p. 68): message loss "and the associated time outs" must be handled; no timeout value.
5. **EV.3.1.3** (p. 93): torque-adjusting algorithms must never increase driver request; no tolerance or time.
6. **EV.3.3.3** (p. 93): no regen at 0-5 km/h; no speed-measurement accuracy or time.
7. **EV.8.3.4 / EV.8.3.5** (p. 107): every BMS detection (over/under-voltage, over-temperature, blown sense fuse, missing/interrupted measurement, BMS fault) must open the SDC; **no detection time, debounce, or reaction time anywhere**.
8. **EV.8.3.4.d** (p. 107): "Missing or interrupted" measurements; no definition of how long missing counts as missing.
9. **EV.7.6.5** (p. 104): BMS detection of an open fusible link; no time.
10. **EV.8.2.1** (p. 106): SDC opens on detection from any listed component; no time (only the downstream 5 s voltage decay and "immediately" for current).
11. **EV.8.2.2.b** (p. 106) and **EV.9.4.2.a** (p. 111): battery current "must stop immediately"; no number (EV.5.4.5 permits up to 250 ms IR hold-up).
12. **EV.8.3.3** (p. 107): balancing forbidden while SDC open; no time to stop balancing after the SDC opens.
13. **EV.9.4.1.b** (p. 111): BMS opens the Charging Shutdown Circuit "if a fault is detected"; no time.
14. **EV.8.7.3** (p. 109): BSPD on open/short sensor input; no time (hardware).
15. **EV.8.6.4/5** (p. 108): IMD isolation failure; no time in EV.8 (IN.10.1.2 tests it at 30 s).
16. **EV.6.1.7.c** (p. 100): TS Status Indicator must show a BMS/IMD fault; no time from fault to flash.
17. **EV.10.7.1** (p. 112): sound when Ready to Drive; no latency from entering RTD to sound start.

---

## Open points

- **Version mismatch (unverified).** The filename says "V1-2" but all 147
  footers say "Version 1.0 1 Sept 2026" and the revision summary (p. 4) lists
  only 1.0. Either the file is 1.0 under a 1.2 name, or 1.2 kept 1.0 footers.
  Confirm against fsaeonline.com before the ESF.
- **Energy Meter User Manual and Rules FAQ** are incorporated by reference
  (GR.4.2.3) but not in the PDF. EM installation, data format, whether it
  exposes anything on CAN, and the meaning of "temperature limits" in
  EV.3.2.6 are **unverified**.
- **"Hard Braking"** (EV.8.7.2) and **"mechanical brakes are engaged"**
  (EV.4.7.1) have no numeric definition; the team sets and documents them.
- **Brake Light trigger** is not specified in T.3.4.
- **Cross-reference errors in the PDF** (rule text is clear, but the pointers
  are wrong; do not chase them):
  - EV.8.3.5.b and EV.8.6.5.b point to "EV.6.1.6e" for the status indicator; EV.6.1.6.e is the mounting item "Easily visible from the front of the vehicle", the fault display itself is EV.6.1.7.c.
  - EV.8.3.1.a cites "EV.12.5" for Tractive System Active; EV.12.5 is Charging, TS Active is EV.10.4.
  - EV.10.2.c and D.12.4.2 cite "EV.10.5" for Ready to Drive; EV.10.5 is OK to Energize, Ready to Drive is EV.10.6.
  - EV.8.8.1 cites EV.5.5.2 for the MSD interlock; the interlock is EV.5.5.3.
  - EV.5.1.2 and EV.7.5.4 cite "EV.3.1" for the Energy Meter; the Energy Meter is EV.3.2.
  - IN.13.1 cites "(EV.8)" for Ready to Drive mode.
  - EV.4.7.1 cites T.3.2.4 for "mechanical brakes are engaged"; by the reconstruction above T.3.2.4 is the 90% regen rule.
- **T.3 rule numbers** on p. 66 are reconstructed (see Source).
- **T.4.3.5.a** requires the "ETC Systems Form" for digital BSE without the
  "(IC Only)" tag that T.4.2.11.a carries; whether EV teams must submit it
  is unclear from the PDF.
