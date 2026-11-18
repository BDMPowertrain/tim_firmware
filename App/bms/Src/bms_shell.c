/* bms_shell.c — the shell commands only the BMS image has: the latest
   Sweep and the analog inputs. shell.c is common code and knows none of
   them; they reach it through shell_register_commands() (ADR 0001).

   Integer formatting only: newlib-nano's printf has no %f in this project. */

#include "bms_shell.h"

#include <stdio.h>

#include "adc_window.h"
#include "bms_step.h"
#include "cell_temp.h"
#include "print.h"
#include "shell.h"
#include "vbat.h"

/* The shell runs one command at a time, so the snapshot lives here rather
   than on shellTask's stack. */
static bms_pack_t pack;

static const char *chain_state_name(bq_chain_state_t s)
{
    switch (s) {
    case BQ_CHAIN_WAKE:       return "WAKE";
    case BQ_CHAIN_STACK_WAKE: return "STACK_WAKE";
    case BQ_CHAIN_STACK_WAIT: return "STACK_WAIT";
    case BQ_CHAIN_ADDRESS:    return "ADDRESS";
    case BQ_CHAIN_SYNC:       return "SYNC";
    case BQ_CHAIN_VERIFY:     return "VERIFY";
    case BQ_CHAIN_CONFIGURE:  return "CONFIGURE";
    case BQ_CHAIN_RUNNING:    return "RUNNING";
    case BQ_CHAIN_RETRY_WAIT: return "RETRY_WAIT";
    default:                  return "?";
    }
}

/* " -12.3" style, 6 characters, or "   bad" outside the sensor's range. */
static void put_temp(char *out, size_t cap, uint16_t mv)
{
    int16_t deci = 0;

    if (!cell_temp_from_mv(mv, &deci)) {
        (void)snprintf(out, cap, "   bad");
        return;
    }
    const int whole = deci / 10;
    const int tenth = (deci < 0) ? -(deci % 10) : deci % 10;
    if (deci < 0 && whole == 0) {
        (void)snprintf(out, cap, "  -0.%d", tenth);
    } else {
        (void)snprintf(out, cap, "%4d.%d", whole, tenth);
    }
}

/* The latest Sweep: per device, its 8 cell voltages in mV and its 8
   sensors in C. Device 1 is next to the bridge. */
static int cmd_cells(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bms_step_read_pack(&pack);
    shell_printf("chain %s, wake pings %lu, sweeps %lu (%lu with a device missing), "
                 "link timeouts %lu, step %lu\r\n",
                 chain_state_name(pack.chain_state), (unsigned long)pack.wake_attempts,
                 (unsigned long)pack.sweeps, (unsigned long)pack.sweeps_missing,
                 (unsigned long)pack.link_timeouts, (unsigned long)pack.step_count);
    /* bit (1 << bms_fault_t) in BMS_Status order (bms_control.h). */
    shell_printf("faults active 0x%03x, seen 0x%03x, low-voltage limit %s\r\n",
                 (unsigned)pack.faults_active, (unsigned)pack.faults_seen,
                 pack.low_voltage_limit ? "yes" : "no");
    if (pack.chain_state != BQ_CHAIN_RUNNING) {
        shell_printf("no Sweep: the chain is not awake\r\n");
        return 0;
    }

    shell_printf("dev    cell 1..8 (mV) / sensor 1..8 (C)\r\n");
    for (unsigned d = 0u; d < BQ_SWEEP_DEVICES; d++) {
        if ((pack.sweep.devices_read & (1u << d)) == 0u) {
            shell_printf("%3u  missing\r\n", d + 1u);
            continue;
        }
        const size_t base = (size_t)d * BQ_SWEEP_CELLS_PER_DEVICE;
        char line[96];
        int n = snprintf(line, sizeof line, "%3u  mV", d + 1u);
        for (size_t c = 0u; c < BQ_SWEEP_CELLS_PER_DEVICE && n > 0 && (size_t)n < sizeof line;
             c++) {
            n += snprintf(&line[n], sizeof line - (size_t)n, " %5u",
                          (unsigned)pack.sweep.cell_mv[base + c]);
        }
        shell_printf("%s\r\n", line);

        n = snprintf(line, sizeof line, "      C");
        for (size_t c = 0u; c < BQ_SWEEP_CELLS_PER_DEVICE && n > 0 && (size_t)n < sizeof line;
             c++) {
            char t[8];
            put_temp(t, sizeof t, pack.sweep.temp_mv[base + c]);
            n += snprintf(&line[n], sizeof line - (size_t)n, "%s", t);
        }
        shell_printf("%s\r\n", line);
    }
    return 0;
}

/* Raw counts as the Sweep step last saw them: reported only (pin map,
   issue 19). The current inputs are reserved for the pack current sensor
   (#29) and mean nothing yet. */
static int cmd_analog(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bms_step_read_pack(&pack);
    const bms_adc_counts_t *a = &pack.adc;
    shell_printf("board_temp %u  current1 %u  current2 %u  vbat %u  vrefint %u"
                 "  (counts; %u = no sample)\r\n",
                 a->board_temp, a->current[0], a->current[1], a->vbat, a->vrefint,
                 (unsigned)ADC_WINDOW_EMPTY);
    return 0;
}

static int cmd_vbat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t raw;
    uint32_t mv;
    if (!vbat_get_reading(&raw, &mv)) {
        shell_printf("vbat: no sample yet\r\n");
        return 0;
    }
    shell_printf("vbat  raw=%lu  mv=%lu  %lu.%02lu V\r\n", (unsigned long)raw,
                 (unsigned long)mv, (unsigned long)(mv / 1000u),
                 (unsigned long)((mv % 1000u) / 10u));
    return 0;
}

static const shell_cmd_t bmsCommands[] = {
    { "cells",  cmd_cells,  "the latest Sweep: cell mV and sensor C per device" },
    { "analog", cmd_analog, "ADC1 counts: board temp, current inputs, VBAT, VREFINT" },
    { "vbat",   cmd_vbat,   "battery voltage" },
};

void bms_shell_register(void)
{
    shell_register_commands(bmsCommands, sizeof(bmsCommands) / sizeof(bmsCommands[0]));
}
