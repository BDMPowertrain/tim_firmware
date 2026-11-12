---
status: accepted
---

# The CAN message database is a DBC, and its generated C is committed

Both images and the Python bench scripts must agree on every frame layout, so the layouts are defined once, in DBC files (one per bus), and nothing is hand-written from them. `cantools` generates the C pack and unpack code that the images link, and the bench scripts load the same DBC files through `cantools` and `python-can`. The generated C is committed to the repo; CI regenerates it and fails if the result differs from what is committed.

## Considered options

- **A home-made YAML schema with our own generator.** Rejected: it means writing and maintaining a generator, and no CAN tool can open the result. A DBC opens in Vehicle Spy and practically every other CAN tool.
- **Hand-written C structs and a matching Python module.** Rejected: two copies that can disagree, which is exactly what the bench tests must not be exposed to.
- **Generating the C at build time.** Rejected: every firmware build would need Python and a pinned `cantools`. Committing the output keeps the firmware build free of that dependency, and the CI check removes the risk of the copy going stale.
- **Importing vendor DBC files for third-party nodes.** Rejected: the Cascadia DBC could not be obtained, and vendor files carry hundreds of signals we never read. Third-party frames are transcribed by hand, only the signals we use, each with a comment citing the manual page, and checked against a bus capture once the hardware is on the bench.

## Consequences

- A frame layout changes in one place, the DBC. The generated C and the bench scripts follow.
- `cantools` is pinned in a `requirements.txt`. Developers need Python only to change the database or run bench tests, not to build an image.
- The generated code is logic: it includes no HAL header and is host-testable.
- One ID plan covers both buses, so an ID means the same frame on either.
