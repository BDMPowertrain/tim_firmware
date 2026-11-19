---
status: accepted
---

# Charge mode comes from the charger's CAN frames, and the CHG_MODE pin is dropped

The BMS enters charge mode when it receives the charger's status frame (0x18FF50E5, every 1 s) and stays there until a power cycle. The charger delivers current only while the BMS sends its request frame (0x1806E5F4, every 1 s) and stops by itself 5 s after the last one (Elcon CAN Specification 2019). The CHG_MODE input on the BMS master is removed.

## Considered options

- **Keep the CHG_MODE wire.** Rejected: a broken wire reads "not charging" and silently disarms the charge-only under-temperature fault while charging continues. Over CAN the failure goes the safe way, because a BMS that does not know it is charging sends no request and the charger does not run.
- **Infer charging from the VCU being absent.** Rejected: the same thing happens in the car with a CAN fault, and the BMS consumes nothing from the VCU.

## Consequences

- Charge mode cannot be read once at power-up; it begins when the charger is first heard.
- The Control bus is 500 kbit/s and the Elcon document states 250 kbit/s. The charger was measured at 500 kbit/s (issue 33), so the pin comes off the BMS master layout with no bit-rate strap.
- The message database gains two 29-bit frames. The charger addresses the BMS as 0xF4, which is also the insulation monitor's default address; they never share a bus, since the charger is only on the hand cart.

## Amendment: the charger's Enable pin belongs to the Charging Shutdown Circuit (issue 33)

The charger delivers current only when two things allow it, and each has its own owner:

- **Enable, pin F of the charger's signal connector** (A CAN L, B CAN H, C CAN GND, D 12V+, E 12V-, F Enable; HK-J manual p.6-7). F is pulled down inside the charger; joined to D, the charger's own 12 V output, it may run, and open, it stops. The Charging Shutdown Circuit joins D to F through a contact of its latching relay. When the circuit opens (Charger Shutdown Button, BMS_OK or IMD), the charger is turned off and stays off until the manual reset, and the charging isolation relays open: EV.9.4.2 a, c and d are met in hardware.
- **The request over CAN.** The BMS starts, sets and stops every charge with CHG_Request alone (control byte, voltage, current; ADR 0019).

Firmware never drives or reads the Enable pin, and no MCU pin is spent on it. The BMS knows the Charging Shutdown Circuit is closed, and so that Enable is on, from its Shutdown Circuit sense input, which reads either circuit. The charger's status frame defines no Enable flag; any flag that changes with Enable is logged as an Event like every other flag change, and no firmware decision depends on it.

Considered: opening the charging isolation relays alone, which leaves the charger on and arguably fails EV.9.4.2 c; and cutting the charger's AC input with a contactor, which is heavier, needs mains switching on the cart and silences the charger on CAN as well. Neither is used.

The bench checks of the charger (obedience, stop times, small requests, broadcast after a stop, Enable, bus-fault recovery) run with the charge-control bench cases (issue 95): the ones that need no load first, the rest at the first charge of the assembled battery.
