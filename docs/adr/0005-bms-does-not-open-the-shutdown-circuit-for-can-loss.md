---
status: accepted
---

# The BMS does not open the Shutdown Circuit for a CAN failure

When the BMS loses the Control bus (bus-off, or no other node answering) it keeps BMS_OK up, carries on monitoring cells, and retries the bus. Its protection never depended on CAN: it makes no decision from any received frame, and BMS_OK is a hard-wired output. The VCU covers the other side, because BMS_Status timing out forces zero torque within 100 ms.

## Considered options

- **Drop BMS_OK on bus-off.** Rejected: it adds a cause for opening the Shutdown Circuit with no safety gain, since the cells are still monitored and the VCU has already stopped torque. It would also stop a charge on the hand cart for a loose CAN connector, where the charger already stops itself on lost communication.

## Consequences

- While the Control bus is down the Energy Meter receives no cell temperatures (0x60D), which officials may treat as missing data. This is accepted.
- The failure must still be visible. The BMS records each bus-off in its stored fault log, keeps live counters readable from the shell, and reports the Control bus state in its status bits on the DAQ bus.
- Bus recovery follows the same schedule as the VCU: restart after 10 ms, and after 3 bus-offs without a clean second in between, one retry per second.
