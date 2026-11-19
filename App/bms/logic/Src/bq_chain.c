/* bq_chain.c — see bq_chain.h. */

#include "bq_chain.h"

#include <string.h>

/* Registers (BQ79616-Q1 data sheet SLUSE81F 7.5.4; the bridge's CONTROL1
   is at the same address, BQ79600-Q1 data sheet SLUSDS1A 7.5.1). */
#define REG_DEV_CONF        0x0002u
#define REG_ACTIVE_CELL     0x0003u
#define REG_OV_THRESH       0x0009u /* then UV_THRESH, OTUT_THRESH */
#define REG_GPIO_CONF1      0x000Eu /* GPIO_CONF1..4, two GPIOs each */
#define REG_FAULT_MSK1      0x0016u /* then FAULT_MSK2 */
#define REG_DIR0_ADDR       0x0306u
#define REG_COMM_CTRL       0x0308u
#define REG_CONTROL1        0x0309u
#define REG_CONTROL2        0x030Au
#define REG_ADC_CTRL1       0x030Du
#define REG_ADC_CTRL2       0x030Eu /* then ADC_CTRL3 */
#define REG_CB_CELL16_CTRL  0x0318u /* to CB_CELL9_CTRL at 0x031F */
#define REG_CB_CELL8_CTRL   0x0320u /* to CB_CELL1_CTRL at 0x0327 */
#define REG_OVUV_CTRL       0x032Cu /* then OTUT_CTRL */
#define REG_BAL_CTRL2       0x032Fu
#define REG_FAULT_RST1      0x0331u /* then FAULT_RST2 */
#define REG_DIAG_CBFET_CTRL1 0x0338u /* then DIAG_CBFET_CTRL2 */
#define REG_DIAG_COMP_CTRL2 0x033Bu
#define REG_DIAG_COMP_CTRL3 0x033Cu
#define REG_OTP_ECC_DATAIN1 0x0343u /* to DATAIN8 at 0x034A */
#define REG_ADC_STAT2       0x0528u
#define REG_BAL_STAT        0x052Bu
#define REG_FAULT_COMP_VCOW1 0x0548u /* then VCOW2 */
#define REG_FAULT_COMP_CBFET1 0x054Eu /* then CBFET2 */
#define REG_DIETEMP1        0x05AEu /* _HI, then _LO */

/* The bridge's (BQ79600-Q1 data sheet SLUSDS1A 7.5). */
#define REG_BRIDGE_DEV_CONF1  0x2001u
#define REG_BRIDGE_FAULT_MSK  0x2020u
#define REG_BRIDGE_FAULT_RST  0x2030u

#define CONTROL1_ADDR_WR    0x01u
#define CONTROL1_SEND_WAKE  0x20u
#define COMM_CTRL_STACK     0x02u
#define COMM_CTRL_TOP       0x03u /* stack device and top of stack */
/* Manual balancing, sample the timers, and stop it on any unmasked fault
   (the chip's stop-on-fault, issue 64). */
#define BAL_CTRL2_BAL_GO    0x22u
#define ACTIVE_CELL_8S      0x02u
#define GPIO_PAIR_ADC_OTUT  0x09u /* 001 = "as ADC and OTUT inputs", both GPIOs */
#define CONTROL2_TSREF_EN   0x01u
#define ADC_CTRL1_CONTINUOUS_GO 0x06u /* MAIN_MODE continuous, MAIN_GO */

/* The comparators (issue 64), SLUSE81F 7.5.4.8. */
#define OV_THR_4225_MV      0x24u /* 0x22 is 4175 mV, 25 mV steps */
#define UV_THR_2400_MV      0x18u /* 0x00 is 1200 mV, 50 mV steps */
/* UT_THR[7:5] 111 = 80 % (the least sensitive; UT is masked), OT_THR[4:0]
   20 = 30 % of TSREF (10 % at 0, 1 % steps): 1500 mV of 5 V, 61.7 C on the
   Enepaq table, where 60 C is 1510 mV and 31 % (1550 mV) is 55 C. */
#define OTUT_THR_OT_30PCT   0xF4u
#define OVUV_OTUT_GO_RR     0x05u /* round robin on every active input, GO */

/* NFAULT (bq_chain.h). FAULT_MSK1: MSK_UT 0x40, MSK_COMP 0x04. FAULT_MSK2:
   MSK_COMM3_FCOMM, _FTONE, _HB, MSK_COMM2, MSK_COMM1. */
#define FAULT_MSK1_STACK    0x44u
#define FAULT_MSK2_STACK    0x1Fu
/* DEV_CONF: NO_ADJ_CB, FCOMM_EN, NFAULT_EN, its reset value, written so
   no OTP setting can differ. */
#define DEV_CONF_STACK      0x54u
/* Bridge DEV_CONF1: FCOMM_EN, NFAULT_EN, its reset value. FAULT_MSK:
   MSK_COML_H, MSK_UART_SPI, MSK_FTONE_DET, MSK_HB. */
#define DEV_CONF1_BRIDGE    0x14u
#define FAULT_MSK_BRIDGE    0xD8u
/* Every fault reset (FAULT_RST2 bit 7 is reserved). */
#define FAULT_RST1_ALL      0xFFu
#define FAULT_RST2_ALL      0x7Fu
#define FAULT_RST_BRIDGE_ALL 0xFFu

/* Balancing (issue 68), SLUSE81F 7.3.3, 7.3.6.4.6.3, 7.5.4.4. */
#define BAL_STAT_INVALID_CBCONF 0x80u
#define CTRL3_CBFET_GO       0x40u /* CBFET_CTRL_GO: the configured check FETs on, the rest off */
#define CTRL3_CBFET_COMPARE  0x09u /* COMP_ADC_SEL 100, COMP_ADC_GO */
#define ADC_CTRL2_AUX_ALL    0x00u /* AUX_CELL_SEL: every active cell */
#define ADC_CTRL3_AUX_CONTINUOUS_GO 0x06u /* AUX_MODE continuous, AUX_GO */
#define ADC_STAT2_DRDY_CBFET 0x02u
#define DIETEMP_NOT_CONVERTED 0x8000u
#define DIETEMP_COUNTS_PER_DC 4 /* 0.025 C a count */

/* The open-wire check (issue 64), SLUSE81F 7.5.4.11. */
#define OVUV_STOP           0x04u /* OVUV_CTRL: GO with OVUV_MODE 00, do not run */
#define OW_THR_2300_MV      0x06u /* 500 mV at 0, 300 mV steps */
#define CTRL3_SINKS_OFF     0x00u
#define CTRL3_VC_SINKS_ON   0x10u /* OW_SNK 01 */
#define CTRL3_VC_COMPARE    0x15u /* OW_SNK 01, COMP_ADC_SEL 010, COMP_ADC_GO */
#define ADC_STAT2_DRDY_VCOW 0x08u
/* The comparison takes a Main ADC round robin, 192 us (SLUSE81F
   tMAIN_ADC_CYCLE); three are waited out. */
#define OPEN_WIRE_COMPARE_US 600u

#define DLL_SYNC_REGS 8u
#define BRIDGE_ADDR   0u
#define TOP_ADDR      ((uint8_t)BQ_SWEEP_DEVICES)

/* SLUAA17A 2.1 step 4: (1.6 ms tone + 10 ms to ACTIVE) x 16 devices. */
#define STACK_WAKE_MS   186u
/* Spec (issue 36): a chain that does not wake is retried once per second. */
#define RETRY_PERIOD_MS 1000u
/* A running chain that reads no device for 1 s is woken again. */
#define SILENT_SWEEPS_MAX 50u

/* Worst cases on the link, for bq_plan_worst_us(). */
#define WAKE_PING_US    2760u /* 914 bytes at 2.66 MHz */
#define WRITE_WORST_US  300u  /* 250 us for SPI_RDY, then 14 bytes */
#define READ_COMMAND_US 50u   /* the read command itself */

/* The plan's next command, cleared; NULL if the plan is full. */
static bq_command_t *next_command(bq_plan_t *plan)
{
    if (plan->count >= BQ_PLAN_MAX) {
        return NULL;
    }
    bq_command_t *cmd = &plan->cmd[plan->count++];
    memset(cmd, 0, sizeof *cmd);
    return cmd;
}

static bq_command_t *add_write(bq_plan_t *plan, bq_request_t type, uint8_t dev, uint16_t reg,
                               const uint8_t *data, size_t n)
{
    bq_command_t *cmd = next_command(plan);
    if (cmd != NULL) {
        cmd->len = (uint8_t)bq_frame_write(cmd->frame, type, dev, reg, data, n);
    }
    return cmd;
}

static bq_command_t *add_byte(bq_plan_t *plan, bq_request_t type, uint8_t dev, uint16_t reg,
                              uint8_t value)
{
    return add_write(plan, type, dev, reg, &value, 1u);
}

static bq_command_t *add_stack_read(bq_plan_t *plan, uint16_t reg, size_t count)
{
    bq_command_t *cmd = next_command(plan);
    if (cmd != NULL) {
        cmd->len = (uint8_t)bq_frame_read(cmd->frame, BQ_STACK_READ, 0u, reg, count);
        cmd->response_len = (uint16_t)(BQ_SWEEP_DEVICES * (count + BQ_RESPONSE_OVERHEAD));
    }
    return cmd;
}

static bq_command_t *add_bridge_read(bq_plan_t *plan, uint16_t reg, size_t count)
{
    bq_command_t *cmd = next_command(plan);
    if (cmd != NULL) {
        cmd->len = (uint8_t)bq_frame_read(cmd->frame, BQ_SINGLE_READ, BRIDGE_ADDR, reg, count);
        cmd->response_len = (uint16_t)(count + BQ_RESPONSE_OVERHEAD);
    }
    return cmd;
}

static void sinks_off(bq_plan_t *plan)
{
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL3, CTRL3_SINKS_OFF);
}

static void plan_address(bq_plan_t *plan)
{
    static const uint8_t zeros[8] = {0};

    /* The balancing stop, first up the chain (spec, issue 36; SLUSE81F
       7.3.3.3.2: every timer to 0 s, then BAL_GO). */
    add_write(plan, BQ_BROADCAST_WRITE, 0u, REG_CB_CELL16_CTRL, zeros, sizeof zeros);
    add_write(plan, BQ_BROADCAST_WRITE, 0u, REG_CB_CELL8_CTRL, zeros, sizeof zeros);
    add_byte(plan, BQ_BROADCAST_WRITE, 0u, REG_BAL_CTRL2, BAL_CTRL2_BAL_GO);

    /* SLUAA17A 2.2.1 steps 1 to 5. */
    for (uint16_t i = 0u; i < DLL_SYNC_REGS; i++) {
        add_byte(plan, BQ_STACK_WRITE, 0u, (uint16_t)(REG_OTP_ECC_DATAIN1 + i), 0x00u);
    }
    add_byte(plan, BQ_BROADCAST_WRITE, 0u, REG_CONTROL1, CONTROL1_ADDR_WR);
    for (uint8_t addr = BRIDGE_ADDR; addr <= TOP_ADDR; addr++) {
        add_byte(plan, BQ_BROADCAST_WRITE, 0u, REG_DIR0_ADDR, addr);
    }
    add_byte(plan, BQ_BROADCAST_WRITE, 0u, REG_COMM_CTRL, COMM_CTRL_STACK);
    add_byte(plan, BQ_SINGLE_WRITE, TOP_ADDR, REG_COMM_CTRL, COMM_CTRL_TOP);
}

static void plan_configure(bq_plan_t *plan)
{
    static const uint8_t gpioConf[4] = {GPIO_PAIR_ADC_OTUT, GPIO_PAIR_ADC_OTUT,
                                        GPIO_PAIR_ADC_OTUT, GPIO_PAIR_ADC_OTUT};
    static const uint8_t thresholds[3] = {OV_THR_4225_MV, UV_THR_2400_MV, OTUT_THR_OT_30PCT};
    static const uint8_t masks[2] = {FAULT_MSK1_STACK, FAULT_MSK2_STACK};

    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_ACTIVE_CELL, ACTIVE_CELL_8S);
    /* A device that kept power through an MCU reset may have been mid-check. */
    sinks_off(plan);
    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_GPIO_CONF1, gpioConf, sizeof gpioConf);
    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_OV_THRESH, thresholds, sizeof thresholds);
    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_FAULT_MSK1, masks, sizeof masks);
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_DEV_CONF, DEV_CONF_STACK);
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL2, OW_THR_2300_MV);
    /* TSREF biases the temperature sensors (research for issue 3, "How the
       boards are wired"); it settles in 6 ms, a step before the OT
       comparator starts and the first Sweep. */
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_CONTROL2, CONTROL2_TSREF_EN);
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_ADC_CTRL1, ADC_CTRL1_CONTINUOUS_GO);
    (void)add_byte(plan, BQ_SINGLE_WRITE, BRIDGE_ADDR, REG_BRIDGE_DEV_CONF1, DEV_CONF1_BRIDGE);
    (void)add_byte(plan, BQ_SINGLE_WRITE, BRIDGE_ADDR, REG_BRIDGE_FAULT_MSK, FAULT_MSK_BRIDGE);
}

/* OVUV_CTRL and OTUT_CTRL: both protectors round robin, started. */
static void plan_protect(bq_plan_t *plan)
{
    static const uint8_t go[2] = {OVUV_OTUT_GO_RR, OVUV_OTUT_GO_RR};

    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_OVUV_CTRL, go, sizeof go);
}

/* The post-wake faults (spec #36): the stack's first, as a stack fault
   left latched would report itself up the chain again. */
static void plan_clear(bq_plan_t *plan)
{
    static const uint8_t resets[2] = {FAULT_RST1_ALL, FAULT_RST2_ALL};

    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_FAULT_RST1, resets, sizeof resets);
    (void)add_byte(plan, BQ_SINGLE_WRITE, BRIDGE_ADDR, REG_BRIDGE_FAULT_RST, FAULT_RST_BRIDGE_ALL);
}

static void ovuv_restart(bq_plan_t *plan)
{
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_OVUV_CTRL, OVUV_OTUT_GO_RR);
}

static void plan_sweep(const bq_chain_t *c, bq_plan_t *plan)
{
    /* What an open-wire check that stopped part way may have left. */
    if (c->check_unfinished) {
        sinks_off(plan);
        ovuv_restart(plan);
    }
    (void)add_stack_read(plan, BQ_SWEEP_REG, BQ_SWEEP_COUNT);
    bq_command_t *bridge = add_bridge_read(plan, BQ_SWEEP_BRIDGE_REG, 1u);
    if (bridge != NULL) {
        bridge->append = true;
    }
}

void bq_chain_init(bq_chain_t *c)
{
    memset(c, 0, sizeof *c);
    c->state = BQ_CHAIN_WAKE;
}

void bq_chain_plan(bq_chain_t *c, uint32_t now_ms, bq_plan_t *plan)
{
    memset(plan, 0, sizeof *plan);

    /* The two waits end here, so a plan is never a step late. */
    if (c->state == BQ_CHAIN_RETRY_WAIT && now_ms - c->attempt_ms >= RETRY_PERIOD_MS) {
        c->state = BQ_CHAIN_WAKE;
    }
    if (c->state == BQ_CHAIN_STACK_WAIT && (int32_t)(now_ms - c->wait_until_ms) >= 0) {
        c->state = BQ_CHAIN_ADDRESS;
    }

    switch (c->state) {
    case BQ_CHAIN_WAKE:
        plan->wake_ping = true;
        break;
    case BQ_CHAIN_STACK_WAKE:
        add_byte(plan, BQ_SINGLE_WRITE, BRIDGE_ADDR, REG_CONTROL1, CONTROL1_SEND_WAKE);
        break;
    case BQ_CHAIN_ADDRESS:
        plan_address(plan);
        break;
    case BQ_CHAIN_SYNC:
        for (uint16_t i = 0u; i < DLL_SYNC_REGS; i++) {
            add_stack_read(plan, (uint16_t)(REG_OTP_ECC_DATAIN1 + i), 1u);
        }
        break;
    case BQ_CHAIN_VERIFY:
        add_stack_read(plan, REG_DIR0_ADDR, 1u);
        break;
    case BQ_CHAIN_CONFIGURE:
        plan_configure(plan);
        break;
    case BQ_CHAIN_PROTECT:
        plan_protect(plan);
        break;
    case BQ_CHAIN_CLEAR:
        plan_clear(plan);
        break;
    case BQ_CHAIN_RUNNING:
        plan_sweep(c, plan);
        break;
    case BQ_CHAIN_STACK_WAIT:
    case BQ_CHAIN_RETRY_WAIT:
    default:
        break;
    }
}

/* True if every stack device answered the address read-back with its own
   address, top first. */
static bool addresses_ok(const uint8_t *resp, size_t len)
{
    size_t pos = 0u;
    bq_response_t r;

    for (uint8_t dev = TOP_ADDR; dev >= 1u; dev--) {
        if (bq_response_next(resp, len, &pos, &r) != BQ_RESPONSE_OK) {
            return false;
        }
        if (r.dev != dev || r.reg != REG_DIR0_ADDR || r.len != 1u || r.data[0] != dev) {
            return false;
        }
    }
    return pos == len;
}

bool bq_chain_done(bq_chain_t *c, uint32_t now_ms, bool ok, const uint8_t *resp, size_t len,
                   bq_sweep_t *sweep)
{
    bq_chain_state_t next = c->state;

    switch (c->state) {
    case BQ_CHAIN_WAKE:
        c->attempts++;
        c->attempt_ms = now_ms;
        next = ok ? BQ_CHAIN_STACK_WAKE : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_STACK_WAKE:
        c->wait_until_ms = now_ms + STACK_WAKE_MS;
        next = ok ? BQ_CHAIN_STACK_WAIT : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_ADDRESS:
        next = ok ? BQ_CHAIN_SYNC : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_SYNC:
        next = ok ? BQ_CHAIN_VERIFY : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_VERIFY:
        next = (ok && addresses_ok(resp, len)) ? BQ_CHAIN_CONFIGURE : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_CONFIGURE:
        c->check_unfinished = false;
        next = ok ? BQ_CHAIN_PROTECT : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_PROTECT:
        next = ok ? BQ_CHAIN_CLEAR : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_CLEAR:
        c->silent_sweeps = 0u;
        next = ok ? BQ_CHAIN_RUNNING : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_RUNNING:
        if (ok) {
            c->check_unfinished = false; /* the writes that undo it went, if planned */
        }
        bq_sweep_decode(resp, len, sweep);
        if (sweep->devices_read != 0u) {
            c->silent_sweeps = 0u;
        } else if (++c->silent_sweeps >= SILENT_SWEEPS_MAX) {
            c->state = BQ_CHAIN_RETRY_WAIT;
        }
        return true;
    case BQ_CHAIN_STACK_WAIT:
    case BQ_CHAIN_RETRY_WAIT:
    default:
        break;
    }
    c->state = next;
    return false;
}

bool bq_chain_plan_open_wire(bq_chain_t *c, bq_plan_t *plan)
{
    memset(plan, 0, sizeof *plan);
    if (c->state != BQ_CHAIN_RUNNING) {
        return false;
    }

    /* The sinks pull an open wire's cell under the UV threshold and its
       neighbour up: OV and UV stop while they are on, or NFAULT would take
       BMS_OK low on one check. OT keeps running; the sinks are on VC pins
       only. */
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_OVUV_CTRL, OVUV_STOP);
    bq_command_t *on = add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL3, CTRL3_VC_SINKS_ON);
    bq_command_t *go = add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL3, CTRL3_VC_COMPARE);
    sinks_off(plan);
    ovuv_restart(plan);
    (void)add_stack_read(plan, REG_ADC_STAT2, 1u);
    bq_command_t *vcow = add_stack_read(plan, REG_FAULT_COMP_VCOW1, 2u);
    /* Then every latched fault, this check's result included, as the wake
       does (plan_clear()): a fault whose cause remains is not cleared
       (SLUSDS1A 7.3.3.1.2), so the chips latch nothing for longer than a
       second beyond its cause. */
    plan_clear(plan);

    if (on != NULL && go != NULL && vcow != NULL) {
        on->delay_us = (uint16_t)(BQ_OPEN_WIRE_SETTLE_MS * 1000u);
        go->delay_us = OPEN_WIRE_COMPARE_US;
        vcow->append = true;
    }
    return true;
}

/* Each device's answer of count bytes (1 or 2) from reg, big-endian, into
   value[d - 1]; a bit in the result for each device whose frame came
   whole. */
static uint16_t read_each(const uint8_t *resp, size_t len, size_t *pos, uint16_t reg,
                          size_t count, uint16_t value[BQ_SWEEP_DEVICES])
{
    uint16_t read = 0u;
    bq_response_t r;

    size_t at = *pos;
    while (bq_response_next(resp, len, &at, &r) == BQ_RESPONSE_OK) {
        if (r.reg != reg) {
            break; /* the next read's answer: left for it */
        }
        *pos = at;
        if (r.dev >= 1u && r.dev <= BQ_SWEEP_DEVICES && r.len == count) {
            value[r.dev - 1u] = (count == 2u) ? (uint16_t)((r.data[0] << 8) | r.data[1])
                                              : r.data[0];
            read |= (uint16_t)(1u << (r.dev - 1u));
        }
    }
    return read;
}

void bq_chain_open_wire_done(bq_chain_t *c, bool ok, const uint8_t *resp, size_t len,
                             bq_open_wire_t *out)
{
    uint16_t stat[BQ_SWEEP_DEVICES] = {0};
    uint16_t vcow[BQ_SWEEP_DEVICES] = {0}; /* VCOW1 high, VCOW2 low */
    size_t pos = 0u;

    memset(out, 0, sizeof *out);
    if (!ok) {
        c->check_unfinished = true;
    }

    /* ADC_STAT2's answers, then FAULT_COMP_VCOW1..2's, each top first.
       VCOW2, the low byte, holds cells 1 to 8; VCOW1, cells 9 to 16, is
       never set with 8 active cells. A frame cut short ends the decoding,
       as nothing after it can be framed. */
    const uint16_t statRead = read_each(resp, len, &pos, REG_ADC_STAT2, 1u, stat);
    const uint16_t vcowRead = read_each(resp, len, &pos, REG_FAULT_COMP_VCOW1, 2u, vcow);
    out->devices_read = statRead & vcowRead;

    for (uint8_t d = 1u; d <= BQ_SWEEP_DEVICES; d++) {
        const uint16_t bit = (uint16_t)(1u << (d - 1u));
        bool failed = false;
        uint16_t cell = 0u;

        const uint8_t open = (uint8_t)vcow[d - 1u];
        if ((vcowRead & bit) != 0u && open != 0u) {
            failed = true;
            for (uint16_t k = 0u; k < BQ_SWEEP_CELLS_PER_DEVICE && cell == 0u; k++) {
                if ((open & (1u << k)) != 0u) {
                    cell = (uint16_t)((d - 1u) * BQ_SWEEP_CELLS_PER_DEVICE + k + 1u);
                }
            }
        } else if ((out->devices_read & bit) != 0u && (stat[d - 1u] & ADC_STAT2_DRDY_VCOW) == 0u) {
            failed = true; /* did not finish: nothing shown whole */
        }
        if (failed) {
            out->verdict = BQ_OPEN_WIRE_FAIL;
            out->device = d;
            out->cell = cell;
            return;
        }
    }
    out->verdict = (out->devices_read == (uint16_t)((1u << BQ_SWEEP_DEVICES) - 1u))
                       ? BQ_OPEN_WIRE_PASS
                       : BQ_OPEN_WIRE_UNKNOWN;
}

/* The FET check's FETs off: none configured, then CBFET_CTRL_GO (SLUSE81F
   7.5.4.11.8). This also leaves the VC sinks off and no comparison chosen. */
static void check_fets_off(bq_plan_t *plan)
{
    static const uint8_t none[2] = {0x00u, 0x00u};

    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_DIAG_CBFET_CTRL1, none, sizeof none);
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL3, CTRL3_CBFET_GO);
}

static void plan_balance_stop(bq_plan_t *plan)
{
    static const uint8_t zeros[8] = {0};

    (void)add_write(plan, BQ_BROADCAST_WRITE, 0u, REG_CB_CELL16_CTRL, zeros, sizeof zeros);
    (void)add_write(plan, BQ_BROADCAST_WRITE, 0u, REG_CB_CELL8_CTRL, zeros, sizeof zeros);
    (void)add_byte(plan, BQ_BROADCAST_WRITE, 0u, REG_BAL_CTRL2, BAL_CTRL2_BAL_GO);
    check_fets_off(plan);
    (void)add_stack_read(plan, REG_DIETEMP1, 2u);
}

/* CB_CELL8_CTRL comes first, so byte k is cell 8 - k. */
static void plan_balance_set(const bq_balance_cmd_t *cmd, bq_plan_t *plan)
{
    for (uint8_t dev = 1u; dev <= BQ_SWEEP_DEVICES; dev++) {
        uint8_t timers[BQ_SWEEP_CELLS_PER_DEVICE];
        for (size_t k = 0u; k < BQ_SWEEP_CELLS_PER_DEVICE; k++) {
            const unsigned cell = BQ_SWEEP_CELLS_PER_DEVICE - 1u - k; /* 0-based */
            timers[k] = ((cmd->cells[dev - 1u] & (1u << cell)) != 0u) ? BQ_BALANCE_TIMER_10S
                                                                       : 0u;
        }
        (void)add_write(plan, BQ_SINGLE_WRITE, dev, REG_CB_CELL8_CTRL, timers, sizeof timers);
    }
    (void)add_byte(plan, BQ_BROADCAST_WRITE, 0u, REG_BAL_CTRL2, BAL_CTRL2_BAL_GO);
    (void)add_stack_read(plan, REG_BAL_STAT, 1u);
}

static void plan_fet_on(const bq_balance_cmd_t *cmd, bq_plan_t *plan)
{
    const uint8_t aux[2] = {ADC_CTRL2_AUX_ALL, ADC_CTRL3_AUX_CONTINUOUS_GO};
    const uint8_t fets[2] = {0x00u, cmd->fet_cells}; /* CBFET16..9, CBFET8..1 */

    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_ADC_CTRL2, aux, sizeof aux);
    (void)add_write(plan, BQ_STACK_WRITE, 0u, REG_DIAG_CBFET_CTRL1, fets, sizeof fets);
    (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL3, CTRL3_CBFET_GO);
}

static void plan_fet_read(bq_plan_t *plan)
{
    check_fets_off(plan);
    (void)add_stack_read(plan, REG_ADC_STAT2, 1u);
    bq_command_t *fails = add_stack_read(plan, REG_FAULT_COMP_CBFET1, 2u);
    if (fails != NULL) {
        fails->append = true;
    }
}

bool bq_chain_plan_balance(const bq_chain_t *c, const bq_balance_cmd_t *cmd, bq_plan_t *plan)
{
    memset(plan, 0, sizeof *plan);
    if (c->state != BQ_CHAIN_RUNNING) {
        return false;
    }
    switch (cmd->op) {
    case BQ_BALANCE_STOP:
        plan_balance_stop(plan);
        return true;
    case BQ_BALANCE_SET:
        plan_balance_set(cmd, plan);
        return true;
    case BQ_BALANCE_FET_ON:
        plan_fet_on(cmd, plan);
        return true;
    case BQ_BALANCE_FET_COMPARE:
        (void)add_byte(plan, BQ_STACK_WRITE, 0u, REG_DIAG_COMP_CTRL3, CTRL3_CBFET_COMPARE);
        return true;
    case BQ_BALANCE_FET_READ:
        plan_fet_read(plan);
        return true;
    case BQ_BALANCE_NONE:
    default:
        return false;
    }
}

/* The FET check's answer: ADC_STAT2's, then FAULT_COMP_CBFET1..2's. */
static void judge_fet_check(const bq_balance_cmd_t *cmd, const uint8_t *resp, size_t len,
                            bq_balance_result_t *out)
{
    uint16_t stat[BQ_SWEEP_DEVICES] = {0};
    uint16_t fails[BQ_SWEEP_DEVICES] = {0}; /* CBFET16..9 high, CBFET8..1 low */
    size_t pos = 0u;

    const uint16_t statRead = read_each(resp, len, &pos, REG_ADC_STAT2, 1u, stat);
    const uint16_t failRead = read_each(resp, len, &pos, REG_FAULT_COMP_CBFET1, 2u, fails);
    out->devices_read = statRead & failRead;

    for (uint8_t d = 1u; d <= BQ_SWEEP_DEVICES; d++) {
        const uint16_t bit = (uint16_t)(1u << (d - 1u));
        /* An aborted comparison sets every bit, CBFET16..9 included, and
           those are never set with 8 active cells. */
        const uint16_t failed = (uint16_t)(fails[d - 1u] & (0xFF00u | cmd->fet_cells));
        bool bad = false;
        uint16_t cell = 0u;

        if ((failRead & bit) != 0u && failed != 0u) {
            bad = true;
            for (uint16_t k = 0u; k < BQ_SWEEP_CELLS_PER_DEVICE && cell == 0u; k++) {
                if ((failed & cmd->fet_cells & (1u << k)) != 0u) {
                    cell = (uint16_t)((d - 1u) * BQ_SWEEP_CELLS_PER_DEVICE + k + 1u);
                }
            }
        } else if ((out->devices_read & bit) != 0u &&
                   (stat[d - 1u] & ADC_STAT2_DRDY_CBFET) == 0u) {
            bad = true; /* did not finish */
        }
        if (bad) {
            out->fet = BQ_FET_CHECK_FAIL;
            out->fet_device = d;
            out->fet_cell = cell;
            return;
        }
    }
    out->fet = (out->devices_read == (uint16_t)((1u << BQ_SWEEP_DEVICES) - 1u))
                   ? BQ_FET_CHECK_PASS
                   : BQ_FET_CHECK_UNKNOWN;
}

void bq_chain_balance_done(const bq_balance_cmd_t *cmd, bool ok, const uint8_t *resp, size_t len,
                           bq_balance_result_t *out)
{
    uint16_t value[BQ_SWEEP_DEVICES] = {0};
    size_t pos = 0u;

    memset(out, 0, sizeof *out);
    out->ok = ok;
    out->fet = BQ_FET_CHECK_UNKNOWN;

    switch (cmd->op) {
    case BQ_BALANCE_STOP: {
        const uint16_t read = read_each(resp, len, &pos, REG_DIETEMP1, 2u, value);
        for (uint8_t d = 0u; d < BQ_SWEEP_DEVICES; d++) {
            if ((read & (1u << d)) != 0u && value[d] != DIETEMP_NOT_CONVERTED) {
                out->die_dc[d] = (int16_t)((int16_t)value[d] / DIETEMP_COUNTS_PER_DC);
                out->devices_read |= (uint16_t)(1u << d);
            }
        }
        break;
    }
    case BQ_BALANCE_SET:
        out->devices_read = read_each(resp, len, &pos, REG_BAL_STAT, 1u, value);
        for (uint8_t d = 0u; d < BQ_SWEEP_DEVICES; d++) {
            if ((out->devices_read & (1u << d)) != 0u &&
                (value[d] & BAL_STAT_INVALID_CBCONF) != 0u) {
                out->rejected |= (uint16_t)(1u << d);
            }
        }
        break;
    case BQ_BALANCE_FET_READ:
        judge_fet_check(cmd, resp, len, out);
        break;
    default: /* writes only: nothing read */
        break;
    }
}

bq_chain_state_t bq_chain_state(const bq_chain_t *c)
{
    return c->state;
}

bool bq_chain_running(const bq_chain_t *c)
{
    return c->state == BQ_CHAIN_RUNNING;
}

uint32_t bq_chain_attempts(const bq_chain_t *c)
{
    return c->attempts;
}

uint32_t bq_read_timeout_us(size_t response_len)
{
    return (BQ_SWEEP_DEVICES - 1u) * 6u + (uint32_t)response_len * 10u + 100u;
}

uint32_t bq_plan_worst_us(const bq_plan_t *plan)
{
    uint32_t us = plan->wake_ping ? WAKE_PING_US : 0u;

    for (size_t i = 0u; i < plan->count; i++) {
        const bq_command_t *cmd = &plan->cmd[i];
        us += (cmd->response_len == 0u)
                  ? WRITE_WORST_US
                  : READ_COMMAND_US + bq_read_timeout_us(cmd->response_len);
        us += bq_delay_worst_us(cmd->delay_us);
    }
    return us;
}

uint32_t bq_delay_worst_us(uint16_t delay_us)
{
    if (delay_us < BQ_DELAY_SLEEP_US) {
        return delay_us; /* waited out exactly */
    }
    /* Whole ticks asleep, rounded up, and one more for the tick the wait
       starts in. */
    return ((delay_us + 999u) / 1000u + 1u) * 1000u;
}

size_t bq_plan_response_len(const bq_plan_t *plan)
{
    size_t end = 0u;
    size_t most = 0u;

    for (size_t i = 0u; i < plan->count; i++) {
        const bq_command_t *cmd = &plan->cmd[i];
        if (cmd->response_len != 0u) {
            end = (cmd->append ? end : 0u) + cmd->response_len;
            most = (end > most) ? end : most;
        }
    }
    return most;
}
