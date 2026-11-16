cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME tim)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Logic libraries (ADR 0002): built with no HAL, RTOS or generated include
# path, and the same ones the host tests link.
add_subdirectory(../../App/common/logic logic/common)
add_subdirectory(../../App/bms/logic logic/bms)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    # App/common/ is compiled into both images, App/bms/ into this one only
    # (ADR 0001). The logger, VBAT and the SPI3 bus lock join when this
    # project configures their peripherals.
    ../../App/common/Src/can_if.c
    ../../App/common/Src/control_bus.c
    ../../App/common/Src/control_step.c
    ../../App/common/Src/fdcan_irq.c
    ../../App/common/Src/flash_bank2.c
    ../../App/common/Src/parameters.c
    ../../App/common/Src/print.c
    ../../App/common/Src/shell.c
    ../../App/common/Src/stats.c
    ../../App/bms/Src/bms_step.c
    ../../App/bms/Src/tasks.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ../../App/common/Inc
    ../../App/bms/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
    logic_common
    logic_bms
)

# Fail the link when the image outgrows its 224 KB of bank 1 (ADR 0014)
set(IMAGE_LIMIT_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/image_limit.ld)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -T ${IMAGE_LIMIT_SCRIPT})
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINK_DEPENDS ${IMAGE_LIMIT_SCRIPT})
