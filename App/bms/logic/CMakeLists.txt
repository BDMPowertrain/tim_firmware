# Logic compiled into the BMS image and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_bms
    DEPENDS
        logic_common
    SOURCES
        Src/bms_tx.c
)
