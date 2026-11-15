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
presets and the host tests on every push and pull request to dev.

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

## Flash
    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg \
      -c "program build/Debug/vcu/tim.elf verify reset exit"

Use build/Debug/bms/tim.elf for a TIM in the BMS role.

## Debug
    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg
    arm-none-eabi-gdb build/Debug/vcu/tim.elf     # .gdbinit connects automatically

## First-time board setup — REQUIRED
A factory-fresh G474 boots into the ST bootloader on this design: PB8 is
I2C1_SCL with a 2k pull-up, and nSWBOOT0 defaults to 1 (BOOT0 from the pin).
Clear it once per board:

    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg \
      -c "init" -c "reset halt" \
      -c "stm32l4x option_write 0 0x20 0x00000000 0x04000000" \
      -c "stm32l4x option_load 0" -c "exit"

Symptom if skipped: flash and verify succeed, nothing runs, PC sits in
0x1FFF____ instead of 0x0800____.

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
  and the host tests. Every logic module gets a host test.
- All task priorities and stack sizes live in the image's tasks.h:
  App/vcu/Inc/tasks.h and App/bms/Inc/tasks.h. One place per image.
- The shell is common code. A command only one image has is registered by
  that image with shell_register_commands() (App/vcu/Src/vcu_shell.c).
- targets/<image>/tim.ioc is the source of truth for that image's pin and
  peripheral config. Both projects must name the same firmware package.
- The BMS project is minimal for now: clock, FreeRTOS, USART1, the heartbeat
  LED and the IMU chip select held high. Its image links print, shell and
  stats from App/common/; the other common modules join when their
  peripherals are configured.

## Conventions
- Tasks are created in the image's tasks.c (App/vcu/Src/tasks.c,
  App/bms/Src/tasks.c), not in CubeMX. CubeMX creates only initTask, which
  calls tasks_init() and then terminates.
- osPriorityRealtime and osPriorityHigh are RESERVED for the image's control
  step (the VCU torque path, the BMS Sweep and its checks) and hard hardware
  deadlines. Do not use them for test or logging tasks.
- LEDs are active LOW (3V3 -> 240R -> LED -> MCU pin).
- SPI3 is shared between the onboard IMU and the mezzanine header — every
  access must take spi3Mutex.
- Use osDelay, never HAL_Delay, inside a task.

## Regeneration ritual
Commit before regenerating in CubeMX. Review `git diff` and commit the
regeneration separately from your own changes.

## Agent skills

### Issue tracker

Issues are tracked in GitHub Issues on BDMPowertrain/tim_firmware (via `gh`). See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (needs-triage, needs-info, ready-for-agent, ready-for-human, wontfix). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
