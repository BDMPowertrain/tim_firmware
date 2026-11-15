# Logic libraries (ADR 0002).
#
# Logic is the code that decides from values it is handed and never touches
# hardware, the RTOS or the clock. Each code area has one logic library:
# logic_common, logic_vcu and logic_bms. The same CMake files are used by the
# image builds (cross compiler) and the host tests (native compiler), so the
# same source goes into both.
#
# A logic library gets its own Inc/ folder as its only include path. It never
# links stm32cubemx, which is what carries the HAL, CMSIS, FreeRTOS and
# generated include paths, so a HAL or RTOS include in logic fails to compile.

include_guard(GLOBAL)

# House rules for logic and its tests: warnings are errors, and float is never
# promoted to double.
function(logic_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wdouble-promotion)
endfunction()

# add_logic_library(<name> [DEPENDS <logic library>...] [SOURCES <file>...])
#
# Call from App/<area>/logic/CMakeLists.txt. Headers go in Inc/ beside it.
# An area with no sources yet gets an empty placeholder (an INTERFACE library)
# so the images and tests can link it from day one.
function(add_logic_library name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")

    if(NOT arg_SOURCES)
        add_library(${name} INTERFACE)
        target_link_libraries(${name} INTERFACE ${arg_DEPENDS})
        return()
    endif()

    add_library(${name} STATIC ${arg_SOURCES})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Inc)
    target_link_libraries(${name} PUBLIC ${arg_DEPENDS})
    logic_warnings(${name})
endfunction()
