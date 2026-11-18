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
 *   ~15    8 cells, GPIOs as ADC inputs, TSREF     the sensors' bias
 *          on, main ADC continuous
 *   ~16-   one Sweep per step
 *
 * The first command that goes up the chain on every wake is a balancing
 * stop (spec, issue 36): every balancing timer to 0 s with BAL_GO, as a
 * broadcast because the stack may not be addressed yet. A device that was
 * bleeding when the MCU reset stops here.
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
    BQ_CHAIN_CONFIGURE,  /* cells, sensors, ADC */
    BQ_CHAIN_RUNNING,    /* one Sweep per step */
    BQ_CHAIN_RETRY_WAIT, /* a wake failed: the next goes a second after it */
} bq_chain_state_t;

/* One command frame and the response bytes it brings back (0 for a write). */
typedef struct {
    uint8_t  frame[BQ_FRAME_MAX];
    uint8_t  len;
    uint16_t response_len;
} bq_command_t;

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
} bq_chain_t;

void bq_chain_init(bq_chain_t *c);

/* This step's plan at now_ms. Calling it again before bq_chain_done()
   gives the same plan. */
void bq_chain_plan(bq_chain_t *c, uint32_t now_ms, bq_plan_t *plan);

/* Reports the plan carried out. ok: the ping and every command went, and
   every read brought back all its response bytes. resp, len: what the last
   read in the plan brought back, whole or not. True if the plan was a
   Sweep; then *sweep is decoded from resp, and a device that did not answer
   is missing in it. Otherwise *sweep is left alone. */
bool bq_chain_done(bq_chain_t *c, uint32_t now_ms, bool ok, const uint8_t *resp, size_t len,
                   bq_sweep_t *sweep);

bq_chain_state_t bq_chain_state(const bq_chain_t *c);
bool bq_chain_running(const bq_chain_t *c);
uint32_t bq_chain_attempts(const bq_chain_t *c);

/* How long to wait for a stack read's response_len bytes before giving up:
   tWAIT_READ_MAX = (n - 1) x 6 us + m x 10 us + 100 us for n = 16 stack
   devices (BQ79600-Q1 data sheet SLUSDS1A p. 23). */
uint32_t bq_read_timeout_us(size_t response_len);

/* The longest the plan can take on the link: each read's timeout, each
   write's SPI_RDY wait (250 us, SLUSDS1A Fig 7-17) and clocking, and the
   ping. */
uint32_t bq_plan_worst_us(const bq_plan_t *plan);

#endif /* BQ_CHAIN_H */
