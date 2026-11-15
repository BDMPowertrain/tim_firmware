cmake_minimum_required(VERSION 3.22)

# Top-level build: one sub-build per image (ADR 0001).
#
# Each image is a complete CubeMX CMake project under targets/<image>/ and is
# configured as its own build tree, because the generated build files assume
# they are the top-level project (CMAKE_SOURCE_DIR locates the linker script,
# CMAKE_PROJECT_NAME names the executable) and CubeMX owns them, so they cannot
# be edited to fit under a shared top level. Hand-written code lives in App/
# (App/common/ for both images, App/<image>/ for one) and is pulled in by the
# image's own CMakeLists.txt.
#
# Building this project builds every image, so the Debug and Release presets
# each build both.

project(tim_firmware LANGUAGES NONE)

# The host-tests presets build the host tests instead of the images: logic
# compiled with the native GCC and run under CTest (ADR 0002).
option(TIM_HOST_TESTS "Build the host tests instead of the images" OFF)
if(TIM_HOST_TESTS)
    enable_testing()
    add_subdirectory(tests/host)
    return()
endif()

include(ExternalProject)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Fails the configure unless every image's CubeMX project names the same
# firmware package. Each project carries its own copy of the HAL and
# FreeRTOS (ADR 0001), so two versions would mean common code is built
# against two different HALs.
function(check_firmware_package image ioc)
    file(STRINGS ${ioc} package_line REGEX "^ProjectManager\\.FirmwarePackage=")
    string(REGEX REPLACE "^[^=]*=" "" package "${package_line}")
    if(package STREQUAL "")
        message(FATAL_ERROR "${ioc} names no firmware package")
    endif()
    get_property(first_image GLOBAL PROPERTY TIM_FIRMWARE_PACKAGE_IMAGE)
    get_property(first_package GLOBAL PROPERTY TIM_FIRMWARE_PACKAGE)
    if(NOT first_image)
        set_property(GLOBAL PROPERTY TIM_FIRMWARE_PACKAGE_IMAGE ${image})
        set_property(GLOBAL PROPERTY TIM_FIRMWARE_PACKAGE "${package}")
    elseif(NOT package STREQUAL first_package)
        message(FATAL_ERROR
            "The CubeMX projects name different firmware packages: "
            "${first_image} has \"${first_package}\", ${image} has \"${package}\". "
            "Migrate one in CubeMX so both match (ADR 0001).")
    endif()
    # Re-run this check when the project file changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ioc})
endfunction()

# Adds the image under targets/<image>/ as a target named <image>.
# Output: <build dir>/<image>/tim.elf
function(add_image image)
    set(image_dir ${CMAKE_CURRENT_SOURCE_DIR}/targets/${image})
    check_firmware_package(${image} ${image_dir}/tim.ioc)
    ExternalProject_Add(${image}
        SOURCE_DIR ${image_dir}
        BINARY_DIR ${CMAKE_BINARY_DIR}/${image}
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${image_dir}/cmake/gcc-arm-none-eabi.cmake
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_ALWAYS TRUE
        USES_TERMINAL_BUILD TRUE
        INSTALL_COMMAND ""
    )
    # The image's objects live in its own build tree, which the top-level
    # clean does not otherwise reach.
    set_property(DIRECTORY APPEND PROPERTY
        ADDITIONAL_CLEAN_FILES ${CMAKE_BINARY_DIR}/${image})
endfunction()

add_image(vcu)
add_image(bms)
