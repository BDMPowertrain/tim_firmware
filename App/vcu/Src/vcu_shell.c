/* vcu_shell.c — the shell commands only the VCU image has: the IMU, the
   pedal and brake inputs and their calibration, and the CAN and VBAT
   modules the BMS image does not link yet. shell.c is common code and knows none of them; they reach it
   through shell_register_commands() (ADR 0001). */

#include "vcu_shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "can_if.h"
#include "control_bus.h"
#include "adc_window.h"
#include "imu.h"
#include "parameters.h"
#include "print.h"
#include "shell.h"
#include "vbat.h"
#include "vcu_calibration.h"
#include "vcu_params.h"
#include "vcu_sensors.h"
#include "vcu_step.h"

/* mg/mdps conversion done in integer math, not float: this project
   links --specs=nano.specs without -u _printf_float (cmake/gcc-arm-none
   -eabi.cmake), so newlib-nano's vsnprintf silently mis-renders %f.
   IMU_GYRO_MDPS_PER_LSB (70) is already a whole number so raw*70 is
   exact; IMU_ACCEL_MG_PER_LSB (0.244) is scaled by 1000 and truncated,
   fine for a glance-at-it shell readout, not for logged data. */
static int cmd_imu(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    imu_sample_t accel;
    imu_sample_t gyro;

    if (imu_get_latest_accel(&accel)) {
        shell_printf("accel  raw x=%6d y=%6d z=%6d  mg x=%6ld y=%6ld z=%6ld\r\n",
                     accel.x, accel.y, accel.z,
                     (long)accel.x * 244 / 1000, (long)accel.y * 244 / 1000, (long)accel.z * 244 / 1000);
    } else {
        shell_printf("accel  no sample yet\r\n");
    }

    if (imu_get_latest_gyro(&gyro)) {
        shell_printf("gyro   raw x=%6d y=%6d z=%6d  mdps x=%6ld y=%6ld z=%6ld\r\n",
                     gyro.x, gyro.y, gyro.z,
                     (long)gyro.x * 70, (long)gyro.y * 70, (long)gyro.z * 70);
    } else {
        shell_printf("gyro   no sample yet\r\n");
    }

    uint16_t pending;
    bool watermarkFlag;
    if (imu_fifo_status(&pending, &watermarkFlag)) {
        shell_printf("fifo   pending=%u watermark_flag=%u\r\n", pending, (unsigned)watermarkFlag);
    }

    return 0;
}

/* Prints the cached accel/gyro sample roughly once per imuTask drain
   cycle (~100 ms, set by IMU_FIFO_WATERMARK in imu.c) until any key is
   pressed. shell_wait_key()'s timeout does double duty as both
   the print-rate timer and the stop-key check, so there's no separate
   polling loop and no busy-waiting — this task is fully blocked between
   prints. The byte that stops the stream is consumed here and never
   reaches shell_dispatch(), so it doesn't echo or get treated as a
   command; the very next line is a fresh `> ` prompt. Raw counts only
   (not mg/mdps) so the stream stays compact enough to actually watch
   scroll by. */
static int cmd_imustream(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    shell_printf("streaming - press any key to stop\r\n");

    for (;;) {
        imu_sample_t accel;
        imu_sample_t gyro;

        if (imu_get_latest_accel(&accel) && imu_get_latest_gyro(&gyro)) {
            shell_printf("accel x=%6d y=%6d z=%6d  gyro x=%6d y=%6d z=%6d\r\n",
                         accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z);
        }

        if (shell_wait_key(100)) {
            break;
        }
    }

    return 0;
}

#define CMD_CAN_USAGE "usage: can status[1|2]|stby2 on|off|send2 <id> <hex> [fd] [brs]|stream2 (termination: term)\r\n"

/* Matches "stby1"/"stby2", "term1"/"term2", etc. against a prefix and
   sets *bus to the trailing 1 or 2. Returns false (bus untouched) if
   tok doesn't match prefix+1 or prefix+2 exactly. */
static bool parse_bus_suffix(const char *tok, const char *prefix, int *bus)
{
    size_t prefixLen = strlen(prefix);

    if (strncmp(tok, prefix, prefixLen) != 0) {
        return false;
    }
    if (strcmp(tok + prefixLen, "1") == 0) {
        *bus = 1;
        return true;
    }
    if (strcmp(tok + prefixLen, "2") == 0) {
        *bus = 2;
        return true;
    }
    return false;
}

static bool hex_nibble(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

/* Decodes a contiguous hex string ("DEADBEEF") into out[]. Returns the
   byte count, or -1 on an odd-length or non-hex string, or one that
   wouldn't fit in out_max bytes. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_max)
{
    size_t hexLen = strlen(hex);
    if ((hexLen % 2u) != 0u) {
        return -1;
    }

    size_t byteCount = hexLen / 2u;
    if (byteCount > out_max) {
        return -1;
    }

    for (size_t i = 0; i < byteCount; i++) {
        uint8_t hi, lo;
        if (!hex_nibble(hex[i * 2u], &hi) || !hex_nibble(hex[i * 2u + 1u], &lo)) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return (int)byteCount;
}

static void can2_print_status(void)
{
    const char *name = "CAN2";
    can_status_t st;

    if (!can_get_status(&st)) {
        shell_printf("%s  status read failed\r\n", name);
        return;
    }

    shell_printf("%s  standby=%u term=%u fd_capable=%u bus_off=%u err_passive=%u\r\n",
                 name, (unsigned)st.standby, (unsigned)st.terminated, (unsigned)st.fd_capable,
                 (unsigned)st.bus_off, (unsigned)st.error_passive);
    shell_printf("     last_err=0x%02lX tx_err=%lu rx_err=%lu\r\n",
                 (unsigned long)st.last_error_code, (unsigned long)st.tx_error_count,
                 (unsigned long)st.rx_error_count);
    shell_printf("     rx=%lu tx=%lu rx_drop=%lu tx_queue_dropped=%lu tx_rejected=%lu\r\n",
                 (unsigned long)st.rx_count, (unsigned long)st.tx_count,
                 (unsigned long)st.rx_drop_count, (unsigned long)st.tx_queue_dropped,
                 (unsigned long)st.tx_rejected);
    shell_printf("     published=%lu bus_offs=%lu restarts=%lu (retry 1/s)\r\n",
                 (unsigned long)st.published, (unsigned long)st.bus_offs,
                 (unsigned long)st.restarts);
}

/* argv layout: {"can", "send<1|2>", "<id hex>", "<data hex>", ["fd"], ["brs"]}.
   id is parsed as hex (CAN-convention, no "0x" prefix, e.g. "123" means
   0x123) and is auto-classified standard/extended by whether it fits
   in 11 bits -- there's no explicit override for forcing extended
   addressing on a value that would also fit as standard, which this
   bring-up tool doesn't need yet. */
static int can_do_send(int argc, char **argv)
{
    if (argc < 4) {
        shell_printf(CMD_CAN_USAGE);
        return 0;
    }

    char *end;
    unsigned long id = strtoul(argv[2], &end, 16);
    if (*end != '\0') {
        shell_printf("can: bad id '%s'\r\n", argv[2]);
        return 0;
    }

    uint8_t data[64];
    int len = hex_decode(argv[3], data, sizeof(data));
    if (len < 0) {
        shell_printf("can: bad hex payload '%s'\r\n", argv[3]);
        return 0;
    }

    bool is_fd = false;
    bool is_brs = false;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "fd") == 0) {
            is_fd = true;
        } else if (strcmp(argv[i], "brs") == 0) {
            is_brs = true;
        }
    }

    bool is_extended = (id > 0x7FFu);

    if (can_send((uint32_t)id, data, (uint8_t)len, is_extended, is_fd, is_brs)) {
        shell_printf("can: queued %d bytes on id 0x%lX\r\n", len, id);
    } else {
        shell_printf("can: send refused (fd requested on a classic-only bus,"
                     " or >8 bytes without 'fd')\r\n");
    }

    return 0;
}

/* Borrows cmd_imustream()'s idiom for the stop-key check
   (shell_wait_key()'s timeout), but prints only on a genuinely
   new frame (can_get_new_rx()'s sequence check) rather than
   re-printing the same cached one every poll -- unlike IMU's
   continuous sampling, CAN traffic is event-driven, so a fixed-rate
   "heartbeat" print was just noise. Still a cached single-frame
   snapshot, not the raw per-frame queue canRxTask drains (a second
   consumer of that queue would race it) -- a burst of several new
   frames within one 100 ms poll interval still only shows the latest
   of them. */
static int can_stream(void)
{
    uint32_t lastSeq = 0;

    shell_printf("streaming - press any key to stop\r\n");

    for (;;) {
        can_frame_t frame;
        if (can_get_new_rx(&frame, &lastSeq)) {
            shell_printf("id=0x%03lX %s%s len=%u data=",
                         (unsigned long)frame.id,
                         frame.is_extended ? "ext " : "std ",
                         frame.is_fd ? (frame.is_brs ? "fd+brs " : "fd ") : "",
                         frame.len);
            for (uint8_t i = 0; i < frame.len; i++) {
                shell_printf("%02X", frame.data[i]);
            }
            shell_printf("\r\n");
        }

        if (shell_wait_key(100)) {
            break;
        }
    }

    return 0;
}

static int cmd_can(int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(CMD_CAN_USAGE);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        control_bus_print_status();
        can2_print_status();
        return 0;
    }

    int bus;

    if (parse_bus_suffix(argv[1], "status", &bus)) {
        if (bus == 1) {
            control_bus_print_status();
        } else {
            can2_print_status();
        }
        return 0;
    }

    if (parse_bus_suffix(argv[1], "term", &bus)) {
        shell_printf("can: termination is `term %d on|off|stored` now, in both images\r\n", bus);
        return 0;
    }

    if ((parse_bus_suffix(argv[1], "stby", &bus)
         || parse_bus_suffix(argv[1], "send", &bus) || parse_bus_suffix(argv[1], "stream", &bus))
        && bus == 1) {
        shell_printf("can: CAN1 is the Control bus; only the control step drives it\r\n");
        return 0;
    }


    if (parse_bus_suffix(argv[1], "stby", &bus)) {
        if (argc < 3) {
            shell_printf(CMD_CAN_USAGE);
        } else if (strcmp(argv[2], "on") == 0) {
            can_set_standby(true);
            shell_printf("can: standby on\r\n");
        } else if (strcmp(argv[2], "off") == 0) {
            can_set_standby(false);
            shell_printf("can: standby off\r\n");
        } else {
            shell_printf(CMD_CAN_USAGE);
        }
        return 0;
    }

    if (parse_bus_suffix(argv[1], "send", &bus)) {
        return can_do_send(argc, argv);
    }

    if (parse_bus_suffix(argv[1], "stream", &bus)) {
        return can_stream();
    }

    shell_printf(CMD_CAN_USAGE);
    return 0;
}

/* mg/mdps-style integer-only display, matching cmd_imu's rationale
   exactly: --specs=nano.specs without -u _printf_float means %f
   mis-renders, so the volts figure is built from plain integer
   division/modulo instead of trusting vsnprintf with a float. */
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

    shell_printf("vbat  raw=%lu  mv=%lu  %lu.%02lu V\r\n",
                 (unsigned long)raw, (unsigned long)mv,
                 (unsigned long)(mv / 1000u), (unsigned long)((mv % 1000u) / 10u));

    return 0;
}

/* Sensor-side millivolts for a count through the 0.6 divider
   (vcu_sensors.h), for reading against a meter. */
static unsigned long sensor_mv(uint16_t counts)
{
    return ((unsigned long)counts * 5u * 3300u) / (3u * 4095u);
}

static void print_raw(const char *name, uint16_t counts)
{
    if (counts == ADC_WINDOW_EMPTY) {
        shell_printf("%-12s raw    -", name);
    } else {
        shell_printf("%-12s raw %4u  %4lu mV", name, (unsigned)counts, sensor_mv(counts));
    }
}

/* What refuses the Start procedure (vcu_control.h). Provisional (#28). */
static void print_start_blockers(uint16_t blockers)
{
    static const struct {
        uint16_t    bit;
        const char *name;
    } names[] = {
        { VCU_START_NOT_TS_ACTIVE,    "not TS_ACTIVE" },
        { VCU_START_BRAKE,            "brake not engaged" },
        { VCU_START_PEDAL,            "pedal pressed" },
        { VCU_START_PEDAL_SENSOR,     "pedal sensor" },
        { VCU_START_BRAKE_SENSOR,     "brake sensor" },
        { VCU_START_PARAMS_DEFAULTED, "parameters defaulted" },
        { VCU_START_MAILBOX,          "mailbox not VALID" },
        { VCU_START_BMS_FAULT,        "BMS fault" },
        { VCU_START_INVERTER_FAULT,   "inverter fault" },
        { VCU_START_SDC_OPEN,         "shutdown circuit open" },
        { VCU_START_BUS_OFF,          "bus-off latched" },
        { VCU_START_BUS_VOLTAGE,      "bus voltage" },
        { VCU_START_TIMING_FAULT,     "timing fault" },
        { VCU_START_TORQUE_CUT,       "torque cut" },
        { VCU_START_NOT_CALIBRATED,   "not calibrated" },
        { VCU_START_PEDAL_CHECK_DUE,  "pedal check due" },
    };

    if (blockers == 0u) {
        shell_printf("ready: a press starts\r\n");
        return;
    }
    shell_printf("refused:");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((blockers & names[i].bit) != 0u) {
            shell_printf(" %s;", names[i].name);
        }
    }
    shell_printf("\r\n");
}

/* The driver request, the torque sent and the Torque cuts active
   (vcu_control.h). Provisional (#28). */
static void print_torque(const vcu_inputs_t *in)
{
    static const char *const cutName[VCU_CUT_COUNT] = {
        [VCU_CUT_APPS]         = "APPS implausible",
        [VCU_CUT_BRAKE_SENSOR] = "brake sensor",
        [VCU_CUT_BRAKE_PEDAL]  = "brake and pedal",
        [VCU_CUT_TIMING]       = "timing fault",
    };
    const int req = in->torque_request;
    const int sent = in->torque;

    shell_printf("torque request %d.%d N.m, sent %d.%d N.m; torque cut", req / 10, req % 10,
                 sent / 10, sent % 10);
    if (in->cuts == 0u) {
        shell_printf(" none\r\n");
        return;
    }
    for (unsigned i = 1u; i < VCU_CUT_COUNT; i++) {
        if ((in->cuts & (1u << i)) != 0u) {
            shell_printf(" %s;", cutName[i]);
        }
    }
    shell_printf("\r\n");
}

/* What the Pedal check (ADR 0016) waits for next. */
static const char *pedal_check_next(const vcu_pedal_check_t *pc)
{
    switch (pc->phase) {
    case VCU_PEDAL_CHECK_RELEASE:       return "release the accelerator";
    case VCU_PEDAL_CHECK_FLOOR_IT:      return "press the accelerator to the floor";
    case VCU_PEDAL_CHECK_RELEASE_AGAIN: return "release the accelerator";
    default:
        return pc->brake_released ? "press the brake" : "release the brake";
    }
}

/* "not calibrated", "parameters defaulted" and "pedal check due", each
   on when it applies (ADR 0015, 0016). Provisional (#28). */
static void print_calibration(const vcu_inputs_t *in)
{
    const vcu_cal_result_t cal = vcu_step_calibration();

    if (cal.reason == VCU_CAL_OK) {
        shell_printf("calibration: calibrated");
    } else {
        shell_printf("calibration: not calibrated: %s%s%s", vcu_cal_sensor_name(cal.sensor),
                     cal.sensor != VCU_CAL_SENSOR_NONE ? " " : "", vcu_cal_reason_name(cal.reason));
    }
    if (parameters_defaulted()) {
        shell_printf("; parameters defaulted");
    }
    if (in->pedal_check_due) {
        shell_printf("; pedal check due: %s (in GLV_ON)", pedal_check_next(&in->pedal_check));
    }
    shell_printf("\r\n");
}

static void print_inputs(const vcu_inputs_t *in)
{
    static const char *const appsName[VCU_APPS_COUNT] = { "APPS1", "APPS2" };
    static const char *const brakeName[VCU_BRAKE_COUNT] = { "brake front", "brake rear" };
    static const char *const supplyName[2] = { "supply 1", "supply 2" };

    for (unsigned i = 0; i < VCU_APPS_COUNT; i++) {
        const vcu_apps_reading_t *r = &in->sensors.apps[i];

        print_raw(appsName[i], in->raw.apps[i]);
        shell_printf("  travel %3u.%u%%  %s\r\n", (unsigned)(r->travel / 10u),
                     (unsigned)(r->travel % 10u), vcu_sensor_status_name(r->status));
    }
    for (unsigned i = 0; i < VCU_BRAKE_COUNT; i++) {
        const vcu_brake_reading_t *r = &in->sensors.brake[i];

        print_raw(brakeName[i], in->raw.brake[i]);
        shell_printf("  pressure %4u  %s\r\n", (unsigned)r->pressure,
                     vcu_sensor_status_name(r->status));
    }
    for (unsigned i = 0; i < 2u; i++) {
        print_raw(supplyName[i], in->raw.supply[i]);
        shell_printf("  %s\r\n", in->sensors.supply_ok[i] ? "ok" : "FAULT");
    }
    shell_printf("shutdown circuit %s, start button %s, samples %s\r\n",
                 in->sdc_closed ? "closed" : "open",
                 in->start_pressed ? "pressed" : "released",
                 in->samples_ready ? "ready" : "NOT ready");
    shell_printf("brake %s; start procedure ", in->brake_engaged ? "engaged" : "released");
    print_start_blockers(in->start_blockers);
    print_torque(in);
    print_calibration(in);
}

#define CMD_INPUTS_USAGE "usage: inputs [stream]\r\n"

/* What the control step read on its last step: averaged counts with the
   millivolts at the sensor connector, Pedal travel and brake pressure
   from the calibration, and each sensor's status. `stream` repeats every
   500 ms until a key, for turning pots on a bare TIM. */
static int cmd_inputs(int argc, char **argv)
{
    const bool stream = argc == 2 && strcmp(argv[1], "stream") == 0;

    if (argc > 2 || (argc == 2 && !stream)) {
        shell_printf(CMD_INPUTS_USAGE);
        return 0;
    }
    if (stream) {
        shell_printf("streaming - press any key to stop\r\n");
    }

    do {
        vcu_inputs_t in;

        if (!vcu_step_read_inputs(&in)) {
            shell_printf("inputs: control step not running\r\n");
            return 0;
        }
        print_inputs(&in);
    } while (stream && !shell_wait_key(500));

    return 0;
}

/* ---- calibrate (ADR 0016) ---------------------------------------------- */

#define CAL_HOLD_MS  500u   /* each position is sampled this long */
#define CAL_WAIT_MS  60000u /* time to get into a position */
#define CAL_USAGE    "usage: calibrate [status]\r\n"

/* Asks for a position, waits for a key, then samples it while it is held.
   False, with the reason printed, if no key came or one came while
   sampling. */
static bool cal_hold(const char *position, vcu_cal_capture_t *c)
{
    shell_printf("%s, hold it, then press a key\r\n", position);
    if (!shell_wait_key(CAL_WAIT_MS)) {
        shell_printf("calibrate: no key in %lu s, nothing saved\r\n",
                     (unsigned long)(CAL_WAIT_MS / 1000u));
        return false;
    }
    vcu_cal_capture_init(c);
    for (uint32_t t = 0; t < CAL_HOLD_MS; t += 10u) {
        vcu_inputs_t in;

        if (!vcu_step_read_inputs(&in)) {
            shell_printf("calibrate: control step not running, nothing saved\r\n");
            return false;
        }
        vcu_cal_capture_add(c, &in.raw);
        if (shell_wait_key(10u)) { /* one step's period between samples */
            shell_printf("calibrate: stopped, nothing saved\r\n");
            return false;
        }
    }
    return true;
}

static void cal_print(const vcu_cal_values_t *v)
{
    shell_printf("APPS1 %ld..%ld, APPS2 %ld..%ld, brake zero %ld front %ld rear, "
                 "brake engaged at %ld above zero\r\n",
                 (long)v->apps_min[0], (long)v->apps_max[0], (long)v->apps_min[1],
                 (long)v->apps_max[1], (long)v->brake_zero[0], (long)v->brake_zero[1],
                 (long)v->brake_engaged);
}

/* The guided capture: three held positions, refused with the reason
   unless the result is Calibrated (vcu_calibration.h), saved through the
   save hook (GLV_ON only) and in force after a reset. End points are
   never typed. */
static int cmd_calibrate(int argc, char **argv)
{
    static vcu_cal_capture_t released; /* static: the shell task's stack is small */
    static vcu_cal_capture_t floored;
    static vcu_cal_capture_t braked;

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        vcu_inputs_t in;

        if (!vcu_step_read_inputs(&in)) {
            shell_printf("calibrate: control step not running\r\n");
            return 0;
        }
        print_calibration(&in);
        return 0;
    }
    if (argc != 1) {
        shell_printf(CAL_USAGE);
        return 0;
    }

    const char *refusal = vcu_step_save_refusal();
    if (refusal != NULL) {
        shell_printf("calibrate: refused: %s\r\n", refusal);
        return 0;
    }
    shell_printf("calibrate: three positions, %u ms each; a key while sampling stops\r\n",
                 (unsigned)CAL_HOLD_MS);
    if (!cal_hold("1/3 release the accelerator and the brake", &released) ||
        !cal_hold("2/3 press the accelerator to the floor", &floored) ||
        !cal_hold("3/3 release the accelerator, press the brake firmly", &braked)) {
        return 0;
    }

    vcu_cal_values_t v;
    const vcu_cal_result_t r = vcu_cal_from_captures(&released, &floored, &braked, &v);

    /* The capture's own refusals come before any value is worked out. */
    if (r.reason != VCU_CAL_NO_SAMPLE && r.reason != VCU_CAL_NOT_STEADY &&
        r.reason != VCU_CAL_APPS_DIRECTION) {
        cal_print(&v);
    }
    if (r.reason != VCU_CAL_OK) {
        shell_printf("calibrate: refused: %s%s%s; nothing saved\r\n",
                     vcu_cal_sensor_name(r.sensor), r.sensor != VCU_CAL_SENSOR_NONE ? " " : "",
                     vcu_cal_reason_name(r.reason));
        return 0;
    }

    static const size_t index[] = {
        VCU_PARAM_APPS1_MIN,   VCU_PARAM_APPS1_MAX,   VCU_PARAM_APPS2_MIN,
        VCU_PARAM_APPS2_MAX,   VCU_PARAM_BRAKE1_ZERO, VCU_PARAM_BRAKE2_ZERO,
        VCU_PARAM_BRAKE_ENGAGED, VCU_PARAM_CAL_REVISION,
    };
    const int32_t value[] = {
        v.apps_min[0],   v.apps_max[0],   v.apps_min[1], v.apps_max[1],
        v.brake_zero[0], v.brake_zero[1], v.brake_engaged, v.revision,
    };
    _Static_assert(sizeof(index) / sizeof(index[0]) == sizeof(value) / sizeof(value[0]),
                   "one value per calibration Parameter");

    const char *failure = parameters_save_values(index, value, sizeof(index) / sizeof(index[0]));
    if (failure != NULL) {
        shell_printf("calibrate: save %s\r\n", failure);
        return 0;
    }
    shell_printf("calibrate: saved, in force after a reset; then do the pedal check\r\n");
    return 0;
}

static const shell_cmd_t vcuCommands[] = {
    { "imu",       cmd_imu,       "latest accel/gyro sample" },
    { "imustream", cmd_imustream, "live accel/gyro stream, any key to stop" },
    { "can",       cmd_can,       "can status[1|2]|stby2 on|off|send2 <id> <hex> [fd] [brs]|stream2" },
    { "vbat",      cmd_vbat,      "battery voltage" },
    { "inputs",    cmd_inputs,    "pedal, brake and supply readings [stream]" },
    { "calibrate", cmd_calibrate, "capture pedal and brake end points [status]" },
};

void vcu_shell_register(void)
{
    shell_register_commands(vcuCommands, sizeof(vcuCommands) / sizeof(vcuCommands[0]));
    shell_register_bench_refusal(vcu_step_bench_refusal);
}
