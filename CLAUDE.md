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

## Flash
    openocd -f interface/stlink-dap.cfg -f target/stm32g4x.cfg \
      -c "program build/Debug/vcu/tim.elf verify reset exit"

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
- targets/vcu/ is the CubeMX project (ADR 0001) and everything in it is
  CubeMX-generated. NEVER edit outside
  /* USER CODE BEGIN */ ... /* USER CODE END */ markers. The one exception is
  targets/vcu/CMakeLists.txt, generated once: it lists the App/ sources.
- The root CMakeLists.txt builds each image as its own build tree; the ELF is
  build/<preset>/vcu/tim.elf.
- App/ is hand-written and CubeMX never touches it. New code goes here.
- All task priorities and stack sizes live in App/Inc/tasks.h. One place.
- targets/vcu/tim.ioc is the source of truth for pin and peripheral config.

## Conventions
- Tasks are created in App/Src/tasks.c, not in CubeMX. CubeMX creates only
  initTask, which calls tasks_init() and then terminates.
- osPriorityRealtime and osPriorityHigh are RESERVED for the VCU torque path
  and hard hardware deadlines. Do not use them for test or logging tasks.
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
