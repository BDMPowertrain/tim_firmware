/* termination.c — see termination.h. */

#include "termination.h"

#include <string.h>

#include "cmsis_os2.h"

#include "bus_term.h"
#include "main.h"
#include "print.h"

#define TERM_USAGE "usage: term [1|2 on|off|stored]\r\n"

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    const char   *name;
} relay_pin_t;

static const relay_pin_t relayPin[TERMINATION_BUS_COUNT] = {
    [TERMINATION_CONTROL_BUS] = { Can_Term_1_GPIO_Port, Can_Term_1_Pin, "CAN1 Control bus" },
    [TERMINATION_DAQ_BUS]     = { Can_Term_2_GPIO_Port, Can_Term_2_Pin, "CAN2 DAQ bus" },
};

/* stored, written and written_ms are set once by initTask before any
   other task runs; relay is changed by the shell's override after that.
   Each is one aligned word or byte. */
static bus_term_t term[TERMINATION_BUS_COUNT];

static void drive(termination_bus_t bus)
{
    HAL_GPIO_WritePin(relayPin[bus].port, relayPin[bus].pin,
                      term[bus].relay ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void termination_write_stored(termination_bus_t bus, bool stored)
{
    if (bus >= TERMINATION_BUS_COUNT) {
        return;
    }
    bus_term_write_stored(&term[bus], stored, osKernelGetTickCount());
    drive(bus);
}

bool termination_settled(termination_bus_t bus, uint32_t now_ms)
{
    return bus < TERMINATION_BUS_COUNT && bus_term_settled(&term[bus], now_ms);
}

void termination_override(termination_bus_t bus, bool on)
{
    if (bus >= TERMINATION_BUS_COUNT) {
        return;
    }
    bus_term_override(&term[bus], on);
    drive(bus);
}

bool termination_relay(termination_bus_t bus)
{
    return bus < TERMINATION_BUS_COUNT && term[bus].relay;
}

static void print_bus(termination_bus_t bus)
{
    const bus_term_t *t = &term[bus];

    shell_printf("%-16s relay %-6s parameter %-6s%s\r\n", relayPin[bus].name,
                 t->relay ? "closed" : "open", t->stored ? "closed" : "open",
                 !t->written            ? "  (not written yet)"
                 : bus_term_overridden(t) ? "  OVERRIDDEN"
                                          : "");
}

int termination_cmd(int argc, char **argv)
{
    if (argc == 1) {
        print_bus(TERMINATION_CONTROL_BUS);
        print_bus(TERMINATION_DAQ_BUS);
        return 0;
    }

    termination_bus_t bus;
    if (argc != 3) {
        shell_printf(TERM_USAGE);
        return 0;
    }
    if (strcmp(argv[1], "1") == 0) {
        bus = TERMINATION_CONTROL_BUS;
    } else if (strcmp(argv[1], "2") == 0) {
        bus = TERMINATION_DAQ_BUS;
    } else {
        shell_printf(TERM_USAGE);
        return 0;
    }

    if (strcmp(argv[2], "on") == 0) {
        termination_override(bus, true);
    } else if (strcmp(argv[2], "off") == 0) {
        termination_override(bus, false);
    } else if (strcmp(argv[2], "stored") == 0) {
        termination_override(bus, term[bus].stored);
    } else {
        shell_printf(TERM_USAGE);
        return 0;
    }
    print_bus(bus);
    if (bus_term_overridden(&term[bus])) {
        shell_printf("bench override until reset; `param set can%c_term` changes the stored value\r\n",
                     argv[1][0]);
    }
    return 0;
}
