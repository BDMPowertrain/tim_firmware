---
status: accepted
---

# A fault either cuts torque or leaves Ready to Drive, and a CAN timeout only cuts torque

The VCU reacts to a problem in one of two ways. A torque cut commands zero torque, stays in Ready to Drive, and ends by itself once the cause is gone and the pedal is released. Leaving Ready to Drive disables the inverter and needs the driver's start procedure again. Problems that can clear by themselves get a torque cut: APPS implausibility, BSE failure, the brake-and-pedal check, and a timed-out BMS or inverter mailbox. Problems that mean the Tractive System is gone or a device has declared a fault leave Ready to Drive: the Shutdown Circuit input open, bus voltage lost, a BMS-reported fault, an inverter-reported fault, and the latched bus-off fault.

The rules only require that motor power stops (T.4.2.5, T.4.3.3, EV.4.7); none requires a latch or a restart for these, so the split is ours to choose.

## Considered options

- **Every fault leaves Ready to Drive.** Rejected: one 100 ms CAN glitch in endurance would force the driver through brake-plus-button while moving, with no safety gain, because torque is already zero for as long as the cause lasts.
- **A FAULT state.** Rejected: the state should say what the car is physically doing (no TS, TS present, driveable). Faults are bits that cut torque or force a transition down.

## Consequences

- A torque cut never escalates to leaving Ready to Drive because it lasts long.
- A mailbox-timeout torque cut also sends Enable=0; a pedal torque cut keeps Enable=1 with zero torque.
- Recovery from every torque cut waits for the higher pedal travel to fall below 5%, so torque never returns under a pressed pedal.
- The dash fault LED blinks for a torque cut and is solid when a restart is needed, so the driver can tell the two apart.
