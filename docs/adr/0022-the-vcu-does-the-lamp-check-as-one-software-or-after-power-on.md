---
status: accepted
---

# The VCU does the Lamp check as one software OR, after a power-on reset only

EV.6.3 asks for a Lamp check of the Tractive System Status Indicator, the Ready to Move Lights, the BMS Indicator Light and the IMD Indicator Light: all together, starting within 1 s of the GLV master switch, on for 2 to 6 s, never preventing an indicator's function. The VCU does it, needing nothing from the BMS, through one logic-level output on an I2C expander spare that the VCU breakout board ORs into all four indicators' drivers. Every one of those indicators keeps its normal function in hardware (the shutdown board's BMS and IMD latches, the Tractive System voltage circuit for the Ready to Move Light), so the VCU can only add light to them, never take it away. The check runs after a power-on reset only (ADR 0011), for 3 s counted from the step that first drives the output, under its own constant, and then stays off until the next power-on reset. Housekeeping owns the expander: it writes every expander output off at every start-up before anything else, then drives the Lamp check output from host-tested logic.

## Considered options

- **A hardware one-shot timed from GLV.** Rejected: new hardware on a board not yet designed, for a check that proves bulbs, not safety. A VCU that is dead or unflashed loses the Lamp check, which is an inspection finding and a visible sign of the dead VCU, never a hidden fault.
- **After every reset, like the fault LED.** Rejected: a VCU reset while driving would hold the Tractive System Status Indicator's red light on solid, which reads as a fault indication (EV.6.1.7 c, EV.13), and would hide the Ready to Move Light's flashing.
- **Ending with the fault LED's lamp check at reset release + 3 s.** Rejected: the output can only come on once the expander is up, so the on-time would shrink towards the 2 s floor by the bring-up time.
- **One output per indicator.** Rejected: none of the four is driven by the VCU in normal function, so separate outputs would only add pins. One output also makes "all together" true by construction. Lights the VCU drives in normal function (the fault LED, any future VCU dash light) have outputs of their own and their own lamp check in software.
- **Driving the expander from the control step.** Rejected: the Lamp check is not a safety output, and I2C transfers do not belong in the Realtime step (ADR 0010).

## Consequences

- The expander does not reset with the MCU, so the off-write at every start-up is what clears a Lamp check cut short by a watchdog reset.
- A starved Housekeeping holds the expander's last state, so a stall during the check leaves the indicators on until it recovers. Background stalled reports it after 1 s.
- The fault LED keeps its own lamp check, 3 s after every reset; the brake light has none.
- The second lamp-check spare stays Reserved I/O.
