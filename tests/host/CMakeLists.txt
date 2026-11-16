# Host tests (ADR 0002): logic compiled with the native GCC and run on the
# development PC or in CI, with no TIM involved. Configured by the host-tests
# presets through the root CMakeLists.txt.
#
#     cmake --preset host-tests
#     cmake --build --preset host-tests
#     ctest --preset host-tests

enable_language(C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# ASan and UBSan, used by CI (the host-tests-sanitize preset). Off by default
# because MinGW GCC on Windows has neither.
option(TIM_HOST_SANITIZE "Build the host tests with ASan and UBSan" OFF)
if(TIM_HOST_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

set(repo_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${repo_root}/cmake/logic.cmake)

# Unity is built without our warning flags: it is third-party code.
add_library(unity STATIC ${repo_root}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${repo_root}/third_party/unity)

# The same logic libraries the images link.
add_subdirectory(${repo_root}/App/common/logic logic/common)
add_subdirectory(${repo_root}/App/vcu/logic logic/vcu)
add_subdirectory(${repo_root}/App/bms/logic logic/bms)

# add_host_test(<name> <logic library> <source>...)
#
# One Unity executable per test file, registered with CTest under <name>.
function(add_host_test name logic_library)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ${logic_library} unity)
    logic_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_host_test(test_e2e_p11 logic_common common/test_e2e_p11.c)
add_host_test(test_can_db logic_common common/test_can_db.c)
add_host_test(test_mailbox logic_common common/test_mailbox.c)
add_host_test(test_control_rx logic_common common/test_control_rx.c)
add_host_test(test_step_timing logic_common common/test_step_timing.c)
add_host_test(test_crc32 logic_common common/test_crc32.c)
add_host_test(test_params logic_common common/test_params.c common/ram_flash.c)
add_host_test(test_vcu_tx logic_vcu vcu/test_vcu_tx.c)
add_host_test(test_vcu_params logic_vcu vcu/test_vcu_params.c)
add_host_test(test_bms_tx logic_bms bms/test_bms_tx.c)
add_host_test(test_bms_params logic_bms bms/test_bms_params.c)
