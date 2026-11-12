---
status: accepted
---

# Logic is HAL-free and host-tested

The rules this firmware must meet (APPS/BSE plausibility, torque limits, BMS fault latching, message timeouts) are decision rules with many edge cases, and proving them only on a board means proving a handful of cases by hand. We keep logic apart from hardware access so the same source that goes into an image also compiles on a development PC and is exercised by host tests on every commit. Logic is any code that includes no HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header and never reads a clock or a pin itself; it is written as step functions that take values and the current time as arguments and return values. The task around each logic module reads the hardware, calls the step, and writes the outputs.

## Considered options

- **Logic calls the hardware through faked functions (mocks).** Rejected: every test must script what each faked call returns and in what order, the tests break when the logic changes how it asks, and the faked HAL is a wide interface with little behind it.
- **Bench testing only.** Rejected: edge cases such as a 99 ms versus 101 ms implausibility cannot be covered by hand, and nothing could be tested before the daughterboards exist.
- **A C++ test framework (GoogleTest, CppUTest).** Rejected: it forces logic headers to be valid C++ and adds a second toolchain language for no gain once mocks are not needed.
- **Relying on review to keep HAL includes out of logic.** Rejected: the build can enforce it for free.

## Consequences

- Logic lives in `App/common/logic/`, `App/vcu/logic/` and `App/bms/logic/`, built as CMake libraries with no HAL include path. A HAL include there fails to compile. The same libraries link into the images and into the host tests.
- Flash is the one place logic reaches hardware through an interface: parameter storage and the fault log use a small read/erase/write interface with a RAM-backed adapter for host tests, because torn writes and power loss can only be exercised that way.
- Host tests use Unity, vendored in `third_party/unity/`, run by CTest, and live in `tests/host/`. A `host-tests` CMake preset builds them with a native GCC and `-Wall -Wextra -Werror`. The GitHub Actions workflow runs them with ASan and UBSan on every push and pull request to `dev`; a failing test blocks the merge.
- Logic uses fixed-width integer types, takes time as a 32-bit millisecond count compared only by subtraction (with a host test across the wrap for every timed module), and uses `float`, never `double` (`-Wdouble-promotion`).
- Every firmware-binding rule has at least one host test that cites the rule number in a comment. Rules only hardware or timing can prove get a bench test instead.
- Images carry no fault-injection code. Bench tests inject faults from outside the board and measure reaction times on the wires.
