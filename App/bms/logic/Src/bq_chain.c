/* bq_chain.c — see bq_chain.h. */

#include "bq_chain.h"

#include <string.h>

/* Registers (BQ79616-Q1 data sheet SLUSE81F 7.5.4; the bridge's CONTROL1
   is at the same address, BQ79600-Q1 data sheet SLUSDS1A 7.5.1). */
#define REG_ACTIVE_CELL     0x0003u
#define REG_GPIO_CONF1      0x000Eu /* GPIO_CONF1..4, two GPIOs each */
#define REG_DIR0_ADDR       0x0306u
#define REG_COMM_CTRL       0x0308u
#define REG_CONTROL1        0x0309u
#define REG_CONTROL2        0x030Au
#define REG_ADC_CTRL1       0x030Du
#define REG_CB_CELL16_CTRL  0x0318u /* to CB_CELL9_CTRL at 0x031F */
#define REG_CB_CELL8_CTRL   0x0320u /* to CB_CELL1_CTRL at 0x0327 */
#define REG_BAL_CTRL2       0x032Fu
#define REG_OTP_ECC_DATAIN1 0x0343u /* to DATAIN8 at 0x034A */

#define CONTROL1_ADDR_WR    0x01u
#define CONTROL1_SEND_WAKE  0x20u
#define COMM_CTRL_STACK     0x02u
#define COMM_CTRL_TOP       0x03u /* stack device and top of stack */
#define BAL_CTRL2_BAL_GO    0x02u /* manual balancing, sample the timers */
#define ACTIVE_CELL_8S      0x02u
#define GPIO_PAIR_ADC_ONLY  0x12u /* 010 = "as ADC only input", both GPIOs */
#define CONTROL2_TSREF_EN   0x01u
#define ADC_CTRL1_CONTINUOUS_GO 0x06u /* MAIN_MODE continuous, MAIN_GO */

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

static void add_write(bq_plan_t *plan, bq_request_t type, uint8_t dev, uint16_t reg,
                      const uint8_t *data, size_t n)
{
    if (plan->count >= BQ_PLAN_MAX) {
        return;
    }
    bq_command_t *cmd = &plan->cmd[plan->count++];
    cmd->len = (uint8_t)bq_frame_write(cmd->frame, type, dev, reg, data, n);
    cmd->response_len = 0u;
}

static void add_byte(bq_plan_t *plan, bq_request_t type, uint8_t dev, uint16_t reg,
                     uint8_t value)
{
    add_write(plan, type, dev, reg, &value, 1u);
}

static void add_stack_read(bq_plan_t *plan, uint16_t reg, size_t count)
{
    if (plan->count >= BQ_PLAN_MAX) {
        return;
    }
    bq_command_t *cmd = &plan->cmd[plan->count++];
    cmd->len = (uint8_t)bq_frame_read(cmd->frame, BQ_STACK_READ, 0u, reg, count);
    cmd->response_len = (uint16_t)(BQ_SWEEP_DEVICES * (count + BQ_RESPONSE_OVERHEAD));
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
    static const uint8_t gpioConf[4] = {GPIO_PAIR_ADC_ONLY, GPIO_PAIR_ADC_ONLY,
                                        GPIO_PAIR_ADC_ONLY, GPIO_PAIR_ADC_ONLY};

    add_byte(plan, BQ_STACK_WRITE, 0u, REG_ACTIVE_CELL, ACTIVE_CELL_8S);
    add_write(plan, BQ_STACK_WRITE, 0u, REG_GPIO_CONF1, gpioConf, sizeof gpioConf);
    /* TSREF biases the temperature sensors (research for issue 3, "How the
       boards are wired"); it settles in 6 ms, well before the first Sweep a
       step later. */
    add_byte(plan, BQ_STACK_WRITE, 0u, REG_CONTROL2, CONTROL2_TSREF_EN);
    add_byte(plan, BQ_STACK_WRITE, 0u, REG_ADC_CTRL1, ADC_CTRL1_CONTINUOUS_GO);
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
    case BQ_CHAIN_RUNNING:
        add_stack_read(plan, BQ_SWEEP_REG, BQ_SWEEP_COUNT);
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
        c->silent_sweeps = 0u;
        next = ok ? BQ_CHAIN_RUNNING : BQ_CHAIN_RETRY_WAIT;
        break;
    case BQ_CHAIN_RUNNING:
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
    }
    return us;
}
