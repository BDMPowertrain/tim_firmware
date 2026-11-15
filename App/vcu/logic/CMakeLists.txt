# Logic compiled into the VCU image and the host tests (ADR 0002).
# No HAL, CMSIS-RTOS, FreeRTOS or CubeMX-generated header may be included here.
# No VCU logic exists yet: list the first source under SOURCES and put its
# header in Inc/.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/logic.cmake)

add_logic_library(logic_vcu
    DEPENDS
        logic_common
)
