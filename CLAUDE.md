# TIM Firmware — Telemetry & IMU Module / VCU

Hardware design (schematics, PCB source) lives in a separate repo:
github.com/BDMPowertrain/tim (Altium/KiCad source, docs/tim.pdf schematic).

## Hardware summary
STM32G474RET3, LQFP64, 512 KB flash, 128 KB RAM, 170 MHz, -40..+125C.
HSE 8 MHz crystal. FreeRTOS via CMSIS-RTOS v2, HAL timebase on TIM6.
Onboard: ASM330LHHG1TR IMU (SPI3), microSD (SPI2), 2x isolated CAN FD
(ADM3057E), mezzanine connectors for daughterboards.

## Build
    cmake --preset Debug
    cmake --build --preset Debug

One preset builds both images (ADR 0001): build/<preset>/vcu/tim.elf and
build/<preset>/bms/tim.elf. Both are named tim.elf, so check the folder
before flashing. CI (.github/workflows/build.yml) runs the Debug and Release
presets and the host tests, and checks the generated CAN code, on every push
and pull request to dev.

The configure fails if the two CubeMX projects name different firmware
packages. The link fails if an image exceeds 224 KB (ADR 0014,
cmake/image_limit.ld).

## Host tests
    cmake --preset host-tests
    cmake --build --preset host-tests
    ctest --preset host-tests

Logic (ADR 0002) compiled with the PC's own GCC and run under CTest with
Unity (third_party/unity/), no TIM involved. Tests live in tests/host/, one
executable per test file, added with add_host_test() in
tests/host/CMakeLists.txt. Warnings are errors, and so is a float promoted
to double.

One-time setup: the host-tests presets need a native `gcc` on the PATH, next
to the Arm toolchain.
- Windows: `winget install BrechtSanders.WinLibs.POSIX.UCRT`, then open a new
  terminal and check `gcc --version`.
- Linux or WSL: `sudo apt install gcc cmake ninja-build`.

CI uses the host-tests-sanitize preset, which adds ASan and UBSan. MinGW GCC
on Windows has neither, so run that preset on Linux or in WSL. Do not share
one build/host-tests folder between Windows and WSL: delete it when switching.

## Message database
    pip install -r requirements.txt
    python can/generate.py

can/control.dbc (Control bus) and can/daq.dbc (DAQ bus) are the Message
database (ADR 0003): the one definition of every CAN frame, with the bit
layouts and the reason for them in the DBC comments. A frame layout changes
there and nowhere else. can/generate.py turns them into
App/common/logic/can_db/, which is committed and is logic. NEVER edit
can_db/ by hand: change the DBC file, run the script, commit both. CI
regenerates and fails on any difference. Python (3.10 or later) is needed
only for this, not to build an image.

The generated structs hold raw signal values (tenths of an amp and so on):
the scale and unit of each signal are in its DBC file. No float or double
scaling helper is generated.

Third-party frames carry only the signals we use, each citing its manual
page. A frame that comes once per module (..._00 to ..._15) gets C for its
_00 copy only; pack any module with it and add to the _00 frame ID.

## Flash
    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg \
      -c "program build/Debug/vcu/tim.elf verify reset exit"

Use build/Debug/bms/tim.elf for a TIM in the BMS role.

## Debug
    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg
    arm-none-eabi-gdb build/Debug/vcu/tim.elf     # .gdbinit connects automatically

## Provisioning — REQUIRED, once per board
Provisioning (ADR 0011, 0012) sets the option bytes before any image is
flashed: nSWBOOT0=0 and nBOOT0=1 (boot from flash whatever the PB8 pull-up
does), brown-out reset at the highest level (about 2.8 V). It leaves
BOOT_LOCK at 0 and the factory dual-bank setting (ADR 0014) alone. The bits,
their reasons and the record of the first run on a board are in
tools/provision.tcl.

    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg \
      -f tools/provision.tcl -c "tim_provision" -c "exit"

The MCU resets when the options load. Then confirm with the read-back, which
prints each setting and fails unless it ends "PROVISIONED":

    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg \
      -f tools/provision.tcl -c "tim_options" -c "exit"

Symptom if skipped: flash and verify succeed, nothing runs, PC sits in
0x1FFF____ instead of 0x0800____ (PB8 is I2C1_SCL with a 2k pull-up, so a
factory-fresh G474 boots the ST bootloader). The brown-out level has no
symptom on the bench: run the read-back.

If the option bytes ever fail to load, the MCU does not restore the factory
values (RM0440 section 3.4.2). It forces read protection to level 1, every
user option bit to 1 (so nSWBOOT0=1 and the board boots the ST bootloader)
and BOR_LEV to its lowest level. tim_provision refuses such a board. Leaving
level 1 mass-erases the flash, so a person decides that, and then provisions
again.

Never mass-erase a calibrated board, or any board holding Parameters or an
Event log. Bank 2 holds the Parameters (pedal and brake calibration) and the
Event log (ADR 0014, 0016). `program` above erases only the pages the image
covers and keeps them. A mass erase (OpenOCD `stm32l4x mass_erase`, a "full
chip erase" in STM32CubeProgrammer) destroys both. A mass erase does not
undo provisioning.

## Code layout — important
- targets/vcu/ and targets/bms/ are the two CubeMX projects, one per image
  (ADR 0001), and everything in them is CubeMX-generated. NEVER edit outside
  /* USER CODE BEGIN */ ... /* USER CODE END */ markers. The one exception is
  targets/<image>/CMakeLists.txt, generated once: it lists the App/ sources.
- The root CMakeLists.txt builds each image as its own build tree; the ELF is
  build/<preset>/<image>/tim.elf.
- App/ is hand-written and CubeMX never touches it. New code goes here, in
  one of three areas (ADR 0001): App/common/ is compiled into both images,
  App/vcu/ and App/bms/ into one each.
- Common code never checks which image it is built into: no #ifdef on the
  image. A file that needs an image switch belongs in App/vcu/ or App/bms/.
- Logic (ADR 0002) lives in App/common/logic/, App/vcu/logic/ and
  App/bms/logic/, each a library (logic_common, logic_vcu, logic_bms) made by
  add_logic_library() from cmake/logic.cmake. Logic includes no HAL,
  CMSIS-RTOS, FreeRTOS or generated header: those paths are not given to it,
  so the include fails to compile. The same libraries link into the images
  and the host tests. Every logic module gets a host test. logic_common also
  holds can_db/, the generated Message database code (see above).
- All task priorities and stack sizes live in the image's tasks.h:
  App/vcu/Inc/tasks.h and App/bms/Inc/tasks.h. One place per image.
- The shell is common code. A command only one image has is registered by
  that image with shell_register_commands() (App/vcu/Src/vcu_shell.c).
- tools/ holds bench scripts that run on the PC against a board, such as
  tools/provision.tcl (OpenOCD). Nothing in it is built into an image.
- targets/<image>/tim.ioc is the source of truth for that image's pin and
  peripheral config. Both projects must name the same firmware package.
- The BMS project is minimal for now: clock, FreeRTOS, USART1, FDCAN1 (the
  Control bus), FDCAN2 (the DAQ bus), the heartbeat LED and the IMU chip
  select held high. Its image links print, shell, stats, the control step
  with its Control bus driver and the DAQ bus driver from App/common/; the
  other common modules join when their peripherals are configured.
- The DAQ bus (CAN2) is sent on only through can_send()
  (App/common/Src/can_if.c): a drop-oldest queue that never waits, drained
  by daqTask, which also runs the 100 ms DAQ publisher. The control step
  never calls into can_if.
- The Control bus (CAN1) is driven only by the image's control step
  (App/common/Src/control_step.c, control_bus.c, ADR 0004 and 0010): no
  task, queue or shell command sends on it.

## Conventions
- Tasks are created in the image's tasks.c (App/vcu/Src/tasks.c,
  App/bms/Src/tasks.c), not in CubeMX. CubeMX creates only initTask, which
  calls tasks_init() and then terminates.
- osPriorityRealtime is the image's control step's alone (the VCU torque
  path, the BMS Sweep and its checks, ADR 0010); osPriorityHigh stays empty
  (ADR 0013). Do not use either for any other task.
- LEDs are active LOW (3V3 -> 240R -> LED -> MCU pin).
- SPI3 is shared between the onboard IMU and the mezzanine header — every
  access must take spi3Mutex.
- Use osDelay, never HAL_Delay, inside a task.

## Regeneration ritual
Commit before regenerating in CubeMX. Review `git diff` and commit the
regeneration separately from your own changes.

The generated Reset_Handler (targets/<image>/startup_stm32g474xx.s, a weak
symbol) is to be replaced by our hand-written one in App/common/, which does
the RAM errata reads first (#49, not yet written). Once it exists, after
every regeneration diff the generated Reset_Handler against ours
(`git diff -- targets/*/startup_stm32g474xx.s`, then read ours) and carry any
change CubeMX made, such as a new init call, into ours in its own commit.

## Agent skills

### Issue tracker

Issues are tracked in GitHub Issues on BDMPowertrain/tim_firmware (via `gh`). See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (needs-triage, needs-info, ready-for-agent, ready-for-human, wontfix). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
