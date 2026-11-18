# Logic compiled into the VCU image and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_vcu
    DEPENDS
        logic_common
    SOURCES
        Src/vcu_calibration.c
        Src/vcu_control.c
        Src/vcu_daq.c
        Src/vcu_params.c
        Src/vcu_pedal_check.c
        Src/vcu_sensors.c
        Src/vcu_tx.c
)
