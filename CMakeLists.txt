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

project(tim_firmware LANGUAGES NONE)

include(ExternalProject)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Adds the image under targets/<image>/ as a target named <image>.
# Output: <build dir>/<image>/tim.elf
function(add_image image)
    set(image_dir ${CMAKE_CURRENT_SOURCE_DIR}/targets/${image})
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
