# Logic compiled into both images and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_common
    SOURCES
        Src/control_rx.c
        Src/e2e_p11.c
        Src/mailbox.c
        can_db/can_control.c
        can_db/can_daq.c
)

# can_db/ is the Message database as C (ADR 0003): generated from can/*.dbc by
# can/generate.py and committed. Never edit it; change the DBC and regenerate.
target_include_directories(logic_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/can_db)
