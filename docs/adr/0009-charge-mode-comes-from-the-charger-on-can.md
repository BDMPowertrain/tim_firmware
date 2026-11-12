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
- The Control bus is 500 kbit/s and the Elcon document states 250 kbit/s. The unit's configured rate must be confirmed or changed before the pin is deleted from the layout.
- The message database gains two 29-bit frames. The charger addresses the BMS as 0xF4, which is also the insulation monitor's default address; they never share a bus, since the charger is only on the hand cart.
