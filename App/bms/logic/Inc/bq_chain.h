/*
 * The cell-monitor chain's sequencer (issue 62): what the BMS Control step
 * sends the BQ79600-Q1 bridge and the 16 BQ79616-Q1 stack devices on each
 * step, from a cold chain to one Sweep per step.
 *
 * Each step the step asks for a plan (bq_chain_plan()), the cell-monitor
 * driver carries it out, and the step hands back how it went
 * (bq_chain_done()). The plan never takes longer than BQ_PLAN_BUDGET_US,
 * well inside the 20 ms step, so the wake is spread over steps and no step
 * holds up the next Kick (ADR 0013):
 *
 *   step   plan                                   then
 *   1      the wake ping (bridge out of SHUTDOWN)  wait at least 3.5 ms
 *   2      CONTROL1[SEND_WAKE] to the bridge       wait 186 ms for the
 *                                                  tone to climb 16 devices
 *   ~12    balancing stop, DLL sync writes,        SLUAA17A 2.2.1 steps 1-5
 *          auto-addressing writes
 *   ~13    the 8 DLL sync reads                    step 6
 *   ~14    read back every address                 a wrong answer fails
 *   ~15    8 cells, GPIOs as ADC and OTUT inputs,  the sensors' bias
 *          comparator thresholds, fault masks,
 *          TSREF on, main ADC continuous
 *   ~16    the comparators start                   TSREF has settled
 *   ~17    every latched fault cleared, stack      post-wake faults
 *          and bridge
 *   ~18-   one Sweep per step: the stack's
 *          readings and the bridge's FAULT_SUMMARY
 *
 * The first command that goes up the chain on every wake is a balancing
 * stop (spec, issue 36): every balancing timer to 0 s with BAL_GO, as a
 * broadcast because the stack may not be addressed yet. A device that was
 * balancing when the MCU reset stops here. Its BAL_GO carries the chip's
 * stop-on-fault (BAL_CTRL2[FLTSTOP_EN], sampled at every BAL_GO), as every
 * balancing start must (issue 64).
 *
 * The comparators (issue 64; spec #36): the stack's own OV, UV and OT
 * protectors are a second layer behind the firmware's judgement, reaching
 * the MCU on NFAULT. OV at 4.225 V, UV at 2.4 V, OT at 30 % of TSREF
 * (1500 mV, 61.7 C: the nearest step above 60 C). An unmasked fault in a
 * stack device rides up the chain in its response frames to the bridge
 * (FCOMM_EN), which pulls NFAULT low; NFAULT is ANDed into BMS_OK by
 * hardware (ADR 0008). What reaches NFAULT is chosen here:
 *   - stack: OV, UV, OT, the protectors' self-tests, system, power and OTP
 *     faults. Masked: UT, a fault in Charge mode only and judged by the
 *     firmware; the ADC comparisons, so the open-wire check's result is
 *     judged by the firmware's two checks in a row and never trips BMS_OK
 *     on one; the link's own faults (COMM1-3), which the firmware sees as
 *     missing measurements.
 *   - bridge: what the stack reported (FCOMM_DET), and its own register,
 *     system and power faults. Masked: the link's own faults, among them
 *     the Comm Clear the driver sends after a read that timed out.
 * The chips latch each fault until it is cleared. The firmware clears them
 * once per wake, after the comparators have started, and again at the end
 * of every open-wire check, once a second. A chip does not clear a fault
 * whose cause remains (SLUSDS1A 7.3.3.1.2), so NFAULT stays low while a
 * cause does and at most a second beyond it: nothing is latched in the
 * firmware (spec #36); the shutdown board's latch keeps the Tractive
 * System off (ADR 0007).
 *
 * The open-wire check (issue 64; EV.8.3.4 b; BQ79616-Q1 data sheet
 * SLUSE81F 7.3.6.4.6.4) is a plan of its own, asked for by the step after
 * its Sweep's work (bq_chain_plan_open_wire()): the OV and UV comparators
 * stopped, every VC pin's current sink on, BQ_OPEN_WIRE_SETTLE_MS for an
 * open wire to drain its filter capacitors, then every cell compared with
 * 2.3 V, the sinks off and the comparators on again, each device asked
 * whether it finished (ADC_STAT2[DRDY_VCOW]) and which cells were under
 * (FAULT_COMP_VCOW1..2), and every latched fault cleared. The sinks pull an
 * open wire's cell under the UV comparator's 2.4 V and its neighbour up,
 * which with the comparators running would take BMS_OK low through NFAULT
 * on one check; stopped, only the firmware's two checks in a row decide.
 * They are off for about 6 ms a second; the firmware's own judgement runs
 * on every Sweep throughout. 2.3 V is under 2.4 V, so a whole wire to a
 * cell read below it is a fault already. A check that stopped part way may
 * have left the sinks on and the comparators stopped, so the next Sweep
 * undoes both first.
 *
 * Any failure during the wake ends the attempt; the next wake ping goes a
 * second after the last one began, for as long as it takes. A running chain
 * whose Sweeps bring back no device at all for 50 steps in a row (1 s) is
 * woken again the same way; a Sweep with only some devices missing is a
 * missing measurement and nothing more. The BMS's states and faults (the 2 s
 * to FAULT, missing measurement) are judged elsewhere from what this
 * reports.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock. Time comes in as
 * now_ms.
 */
#ifndef BQ_CHAIN_H
#define BQ_CHAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bq_frame.h"
#include "bq_sweep.h"

typedef enum {
    BQ_CHAIN_WAKE,       /* next: the wake ping */
    BQ_CHAIN_STACK_WAKE, /* next: SEND_WAKE */
    BQ_CHAIN_STACK_WAIT, /* the wake tone is climbing the stack */
    BQ_CHAIN_ADDRESS,    /* balancing stop and auto-addressing writes */
    BQ_CHAIN_SYNC,       /* DLL sync reads */
    BQ_CHAIN_VERIFY,     /* address read-back */
    BQ_CHAIN_CONFIGURE,  /* cells, sensors, thresholds, masks, ADC */
    BQ_CHAIN_PROTECT,    /* the comparators start */
    BQ_CHAIN_CLEAR,      /* the post-wake faults cleared */
    BQ_CHAIN_RUNNING,    /* one Sweep per step */
    BQ_CHAIN_RETRY_WAIT, /* a wake failed: the next goes a second after it */
} bq_chain_state_t;

/* One command frame and the response bytes it brings back (0 for a write). */
typedef struct {
    uint8_t  frame[BQ_FRAME_MAX];
    uint8_t  len;
    uint16_t response_len;
    /* A read's response goes after the one before it in the plan, not over
       it. */
    bool     append;
    /* The link waits at least this long after the command: asleep for a
       wait of BQ_DELAY_SLEEP_US or more, on the cycle counter for less. */
    uint16_t delay_us;
} bq_command_t;

/* A delay this long or longer sleeps whole RTOS ticks. */
#define BQ_DELAY_SLEEP_US 1000u

/* The most response bytes one plan brings back, appended reads together:
   the Sweep's. */
#define BQ_RESPONSE_MAX BQ_SWEEP_TOTAL_LEN

/* The most commands in one step's plan: the addressing step's 31. */
#define BQ_PLAN_MAX 31u

typedef struct {
    bool         wake_ping; /* first, before any command */
    uint8_t      count;
    bq_command_t cmd[BQ_PLAN_MAX];
} bq_plan_t;

/* The longest any plan may take on the link, worst case. */
#define BQ_PLAN_BUDGET_US 15000u

typedef struct {
    bq_chain_state_t state;
    uint32_t attempt_ms;    /* when the current wake attempt's ping went */
    uint32_t wait_until_ms; /* end of BQ_CHAIN_STACK_WAIT */
    uint32_t attempts;      /* wake pings sent since power-on */
    uint32_t silent_sweeps; /* Sweeps in a row that read no device */
    bool     check_unfinished; /* an open-wire check stopped part way */
} bq_chain_t;

void bq_chain_init(bq_chain_t *c);

/* This step's plan at now_ms. Calling it again before bq_chain_done()
   gives the same plan. */
void bq_chain_plan(bq_chain_t *c, uint32_t now_ms, bq_plan_t *plan);

/* Reports the plan carried out. ok: the ping and every command went, and
   every read brought back all its response bytes. resp, len: what the last
   read in the plan brought back, whole or not, after the responses of the
   reads it is appended to. True if the plan was a
   Sweep; then *sweep is decoded from resp, and a device that did not answer
   is missing in it. Otherwise *sweep is left alone. */
bool bq_chain_done(bq_chain_t *c, uint32_t now_ms, bool ok, const uint8_t *resp, size_t len,
                   bq_sweep_t *sweep);

/* How long the sinks are on before the comparison: an open wire drains
   the 0.47 uF filter capacitors on both sides of its pin (the cell board's)
   at the sink's least 380 uA (SLUSE81F IOW_SINK), 0.40 V/ms, so a full
   4.2 V cell reads under 2.3 V within 4.7 ms. To be confirmed on the bench
   with a wire pulled. */
#define BQ_OPEN_WIRE_SETTLE_MS 5u

typedef enum {
    BQ_OPEN_WIRE_PASS,    /* every device finished and found every cell over 2.3 V */
    BQ_OPEN_WIRE_FAIL,    /* a cell under, or a device that did not finish */
    BQ_OPEN_WIRE_UNKNOWN, /* a device did not answer, and none failed */
} bq_open_wire_verdict_t;

typedef struct {
    bq_open_wire_verdict_t verdict;
    uint8_t  device;       /* the first that failed, 1 to 16; 0 for none */
    uint16_t cell;         /* its first cell under, 1 to 128; 0 if it did not finish */
    uint16_t devices_read; /* bit d - 1 set: device d answered both reads */
} bq_open_wire_t;

/* The open-wire check's plan, for after this step's Sweep. False, with an
   empty plan, unless the chain is running. */
bool bq_chain_plan_open_wire(bq_chain_t *c, bq_plan_t *plan);

/* Reports the check's plan carried out, as bq_chain_done() does, and
   judges it into *out. */
void bq_chain_open_wire_done(bq_chain_t *c, bool ok, const uint8_t *resp, size_t len,
                             bq_open_wire_t *out);

bq_chain_state_t bq_chain_state(const bq_chain_t *c);
bool bq_chain_running(const bq_chain_t *c);
uint32_t bq_chain_attempts(const bq_chain_t *c);

/* How long to wait for a stack read's response_len bytes before giving up:
   tWAIT_READ_MAX = (n - 1) x 6 us + m x 10 us + 100 us for n = 16 stack
   devices (BQ79600-Q1 data sheet SLUSDS1A p. 23). */
uint32_t bq_read_timeout_us(size_t response_len);

/* The longest the plan can take on the link: each read's timeout, each
   write's SPI_RDY wait (250 us, SLUSDS1A Fig 7-17) and clocking, each
   delay and a tick more, and the ping. */
uint32_t bq_plan_worst_us(const bq_plan_t *plan);

/* The longest a command's delay can take: exact under BQ_DELAY_SLEEP_US;
   above it, whole ticks rounded up and one more. */
uint32_t bq_delay_worst_us(uint16_t delay_us);

/* The most response bytes the plan's reads leave in the buffer at once:
   each read's, after those it is appended to. */
size_t bq_plan_response_len(const bq_plan_t *plan);

#endif /* BQ_CHAIN_H */
