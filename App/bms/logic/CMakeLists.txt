# Logic compiled into the BMS image and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_bms
    DEPENDS
        logic_common
    SOURCES
        Src/bms_control.c
        Src/bms_daq.c
        Src/bms_params.c
        Src/bms_tx.c
        Src/bq_chain.c
        Src/bq_frame.c
        Src/bq_sweep.c
        Src/cell_temp.c
        Src/cell_temp_table.c
)
