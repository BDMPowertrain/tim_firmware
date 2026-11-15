# Logic compiled into the BMS image and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
# No BMS logic exists yet: list the first source under SOURCES and put its
# header in Inc/.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_bms
    DEPENDS
        logic_common
)
