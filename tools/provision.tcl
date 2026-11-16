# Provisioning: the once-per-board option settings of a TIM (GLOSSARY.md,
# ADR 0011, 0012, 0014). Loaded by OpenOCD after the interface and target
# scripts; see "Provisioning" in CLAUDE.md for the two commands.
#
# HARDWARE STATUS: run on the bench TIM on 2026-11-16 (ST-Link V3, OpenOCD
# 0.12.0+dev, image already flashed and running).
#   Before: FLASH_OPTR 0xFBEFF8AA. nSWBOOT0 was already 0 (from the old
#           first-time step) and BOR_LEV was 0, so the read-back said NOT
#           PROVISIONED.
#   tim_provision wrote and loaded the options. OpenOCD stayed connected
#           through the option-load reset and exited 0.
#   After:  FLASH_OPTR 0xFBEFFCAA, FLASH_SEC1R 0xFFFEFF00 (reserved bits read
#           1, BOOT_LOCK 0, SEC_SIZE1 0), RDP 0xAA, dual-bank. The read-back
#           said PROVISIONED. The image ran from flash after the reset (PC
#           0x0800766C, on a task stack).
#   A second tim_provision said "Already provisioned; nothing written."
#   Not shown: the brown-out reset tripping near 2.8 V. That needs a variable
#   supply and is a bench case.
#
# What it sets, in FLASH_OPTR (0x4002_2020, RM0440 Rev 9 section 3.7.8):
#   nSWBOOT0 (bit 26) = 0  BOOT0 comes from nBOOT0, not from pin PB8. PB8 is
#                          I2C1_SCL with a 2k pull-up, so with the factory
#                          value 1 every reset boots the ST bootloader.
#   nBOOT0   (bit 27) = 1  with nSWBOOT0 = 0: boot from main flash
#                          (RM0440 Table 5, nBOOT1 is then ignored).
#   BOR_LEV  (10:8)   = 4  brown-out reset at about 2.8 V (RM0440; DS12288
#                          gives VBOR4 2.85 to 2.95 V), above the SD card's
#                          2.7 V minimum (ADR 0011).
# What it leaves alone, and checks on read-back:
#   DBANK    (bit 22) = 1  factory dual-bank, 2 x 256 KB (ADR 0014).
#   BOOT_LOCK (FLASH_SEC1R 0x4002_2070 bit 16) = 0 (ADR 0012).
# Every other option bit keeps its value: the write is masked.

set TIM_FLASH_BASE    0x40022000
set TIM_OPTR_OFFSET   0x20
set TIM_SEC1R_OFFSET  0x70

# Value and mask for "stm32l4x option_write": nBOOT0=1, nSWBOOT0=0, BOR_LEV=4.
# The read-back takes its wanted values from these, so they are the one copy.
set TIM_OPTR_VALUE  0x08000400
set TIM_OPTR_MASK   0x0C000700

# FLASH_OPTR fields as {name shift width note}.
set TIM_OPTR_FIELDS {
	{nSWBOOT0 26 1 "boot pin PB8 ignored"}
	{nBOOT0   27 1 "boot from main flash"}
	{BOR_LEV   8 3 "brown-out at about 2.8 V"}
	{DBANK    22 1 "dual-bank, left at factory value"}
}
set TIM_RDP_LEVEL0 0xAA

proc tim_read_flash_reg {offset} {
	global TIM_FLASH_BASE
	return [lindex [read_memory [expr {$TIM_FLASH_BASE + $offset}] 32 1] 0]
}

proc tim_field {reg shift width} {
	return [expr {($reg >> $shift) & ((1 << $width) - 1)}]
}

proc tim_rdp_is_level0 {optr} {
	global TIM_RDP_LEVEL0
	return [expr {($optr & 0xFF) == $TIM_RDP_LEVEL0}]
}

# Prints each setting against what provisioning wants and returns 1 when the
# board is provisioned. Takes the raw register values so it can be tried
# without a board.
proc tim_decode_options {optr sec1r} {
	global TIM_OPTR_FIELDS TIM_OPTR_VALUE TIM_OPTR_MASK

	echo [format "FLASH_OPTR  = 0x%08X" $optr]
	echo [format "FLASH_SEC1R = 0x%08X" $sec1r]

	# A field inside the write mask wants its value from TIM_OPTR_VALUE.
	# DBANK is outside the mask and wants its factory 1; BOOT_LOCK its 0.
	set rows {}
	foreach f $TIM_OPTR_FIELDS {
		lassign $f name shift width note
		if {[tim_field $TIM_OPTR_MASK $shift $width] != 0} {
			set want [tim_field $TIM_OPTR_VALUE $shift $width]
		} else {
			set want 1
		}
		lappend rows $name [tim_field $optr $shift $width] $want $note
	}
	lappend rows BOOT_LOCK [tim_field $sec1r 16 1] 0 \
		"bootloader allowed, left at factory value"

	set ok 1
	foreach {name have want note} $rows {
		if {$have == $want} {
			set mark "ok   "
		} else {
			set mark "WRONG"
			set ok 0
		}
		echo [format "  %s %-9s = %d (want %d: %s)" $mark $name $have $want $note]
	}

	if {[tim_rdp_is_level0 $optr]} {
		echo "        RDP       = 0xAA (level 0, no read protection)"
	} else {
		echo [format "        RDP       = 0x%02X (NOT level 0)" [expr {$optr & 0xFF}]]
	}

	if {$ok} {
		echo "TIM option settings: PROVISIONED"
	} else {
		echo "TIM option settings: NOT PROVISIONED"
	}
	return $ok
}

# Read-back. Reads the live registers without halting or resetting the board,
# and fails (non-zero OpenOCD exit) when the board is not provisioned.
proc tim_options {} {
	global TIM_OPTR_OFFSET TIM_SEC1R_OFFSET
	init
	set optr  [tim_read_flash_reg $TIM_OPTR_OFFSET]
	set sec1r [tim_read_flash_reg $TIM_SEC1R_OFFSET]
	if {![tim_decode_options $optr $sec1r]} {
		error "this TIM is not provisioned"
	}
}

# Provisioning. Halts the board, writes the three settings and reloads the
# option bytes, which resets the MCU. Run tim_options afterwards to confirm.
proc tim_provision {} {
	global TIM_OPTR_OFFSET TIM_SEC1R_OFFSET TIM_OPTR_VALUE TIM_OPTR_MASK
	init
	reset halt

	set optr  [tim_read_flash_reg $TIM_OPTR_OFFSET]
	set sec1r [tim_read_flash_reg $TIM_SEC1R_OFFSET]
	echo "Before:"
	if {[tim_decode_options $optr $sec1r]} {
		echo "Already provisioned; nothing written."
		return
	}
	# RDP level 1 means a failed option load (RM0440 section 3.4.2) or a
	# deliberate lock. Leaving it needs a mass erase: a person decides that.
	if {![tim_rdp_is_level0 $optr]} {
		error "read protection is not level 0; provision only an unprotected TIM"
	}

	stm32l4x option_write 0 $TIM_OPTR_OFFSET $TIM_OPTR_VALUE $TIM_OPTR_MASK
	echo "Written. Loading the option bytes; the MCU resets now."
	stm32l4x option_load 0
	echo "Done. Confirm with the read-back command (tim_options)."
}

# Ends the script with an empty result, so OpenOCD does not print the last
# proc name when it loads this file.
return ""
