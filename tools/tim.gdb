# tim.gdb — attach GDB to a running TIM through OpenOCD, without flashing
# or resetting it. Run OpenOCD first (CLAUDE.md, Debug), then:
#
#     arm-none-eabi-gdb -x tools/tim.gdb build/Debug/vcu/tim.elf
#
# The crash routine (App/common/Inc/crash.h) never stops by itself: it
# always resets, because the core's debug enable bit outlives the probe and
# a halt with nobody attached would freeze the board. This breakpoint is
# how a debugging session stops at a crash instead, with the Crash record
# already written. `bt` shows where it came from; `continue` resets.

target extended-remote :3333
hbreak crash_reset
