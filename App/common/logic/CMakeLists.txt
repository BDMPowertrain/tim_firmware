# Logic compiled into both images and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_common
    SOURCES
        Src/adc_window.c
        Src/bus_recovery.c
        Src/bus_term.c
        Src/control_rx.c
        Src/crc16.c
        Src/crc32.c
        Src/cpu_share.c
        Src/daq_retry.c
        Src/daq_status.c
        Src/e2e_p11.c
        Src/event_codes.c
        Src/event_flood.c
        Src/event_log.c
        Src/frame_queue.c
        Src/heartbeat.c
        Src/kick_gate.c
        Src/mailbox.c
        Src/overrun_watch.c
        Src/params.c
        Src/reset_cause.c
        Src/reset_record.c
        Src/stall_watch.c
        Src/step_timing.c
        can_db/can_control.c
        can_db/can_daq.c
)

# can_db/ is the Message database as C (ADR 0003): generated from can/*.dbc by
# can/generate.py and committed. Never edit it; change the DBC and regenerate.
target_include_directories(logic_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/can_db)
