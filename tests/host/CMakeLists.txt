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
add_host_test(test_bus_recovery logic_common common/test_bus_recovery.c)
add_host_test(test_crc32 logic_common common/test_crc32.c)
add_host_test(test_crc16 logic_common common/test_crc16.c)
add_host_test(test_event_codes logic_common common/test_event_codes.c)
add_host_test(test_event_flood logic_common common/test_event_flood.c)
add_host_test(test_event_log logic_common common/test_event_log.c common/ram_flash.c)
add_host_test(test_pedal_check_record logic_common common/test_pedal_check_record.c common/ram_flash.c)
add_host_test(test_params logic_common common/test_params.c common/ram_flash.c)
add_host_test(test_frame_queue logic_common common/test_frame_queue.c)
add_host_test(test_daq_retry logic_common common/test_daq_retry.c)
add_host_test(test_daq_status logic_common common/test_daq_status.c)
add_host_test(test_bus_term logic_common common/test_bus_term.c)
add_host_test(test_heartbeat logic_common common/test_heartbeat.c)
add_host_test(test_stall_watch logic_common common/test_stall_watch.c)
add_host_test(test_cpu_share logic_common common/test_cpu_share.c)
add_host_test(test_kick_gate logic_common common/test_kick_gate.c)
add_host_test(test_overrun_watch logic_common common/test_overrun_watch.c)
add_host_test(test_adc_window logic_common common/test_adc_window.c)
add_host_test(test_reset_cause logic_common common/test_reset_cause.c)
add_host_test(test_reset_record logic_common common/test_reset_record.c)
add_host_test(test_vcu_tx logic_vcu vcu/test_vcu_tx.c)
add_host_test(test_vcu_daq logic_vcu vcu/test_vcu_daq.c)
add_host_test(test_bms_daq logic_bms bms/test_bms_daq.c)
add_host_test(test_vcu_params logic_vcu vcu/test_vcu_params.c)
add_host_test(test_vcu_sensors logic_vcu vcu/test_vcu_sensors.c)
add_host_test(test_vcu_control logic_vcu vcu/test_vcu_control.c)
add_host_test(test_vcu_calibration logic_vcu vcu/test_vcu_calibration.c)
add_host_test(test_vcu_power_limiter logic_vcu vcu/test_vcu_power_limiter.c)
add_host_test(test_vcu_power_limiter_plant logic_vcu vcu/test_vcu_power_limiter_plant.c)
add_host_test(test_vcu_log logic_vcu vcu/test_vcu_log.c)
add_host_test(test_vcu_imu_log logic_vcu vcu/test_vcu_imu_log.c)
add_host_test(test_bms_tx logic_bms bms/test_bms_tx.c)
add_host_test(test_bms_params logic_bms bms/test_bms_params.c)
add_host_test(test_bq_frame logic_bms bms/test_bq_frame.c)
add_host_test(test_cell_temp logic_bms bms/test_cell_temp.c)
add_host_test(test_bq_sweep logic_bms bms/test_bq_sweep.c)
add_host_test(test_bq_chain logic_bms bms/test_bq_chain.c)
add_host_test(test_bms_control logic_bms bms/test_bms_control.c)
add_host_test(test_record_ring logic_common common/test_record_ring.c)
add_host_test(test_data_log_record logic_common common/test_data_log_record.c)
add_host_test(test_log_batch logic_common common/test_log_batch.c)
add_host_test(test_data_log_card logic_common common/test_data_log_card.c)
add_host_test(test_sd_proto logic_common common/test_sd_proto.c)

# The Python decoder against the sample file test_data_log_record writes
# (issue 53). Python 3.10 or later, standard library only. Required in CI
# (the host-tests-sanitize preset), so the check cannot drop out there.
option(TIM_REQUIRE_PYTHON "Fail the configure without Python for the decoder test" OFF)
if(TIM_REQUIRE_PYTHON)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
else()
    find_package(Python3 3.10 COMPONENTS Interpreter)
endif()
if(Python3_Interpreter_FOUND)
    set_tests_properties(test_data_log_record PROPERTIES FIXTURES_SETUP data_log_sample)
    add_test(NAME test_data_log_decoder
        COMMAND ${CMAKE_COMMAND}
            -DPYTHON=${Python3_EXECUTABLE}
            -DDECODER=${repo_root}/tools/data_log_decode.py
            -DDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/data_log_decoder_check.cmake)
    set_tests_properties(test_data_log_decoder PROPERTIES FIXTURES_REQUIRED data_log_sample)

    # The 80 kW rule's two windows (issue 60): the check itself, then the
    # traces the Power limiter's closed-loop test writes.
    add_test(NAME test_power_rule_windows
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_power_rule_windows.py)
    set_tests_properties(test_vcu_power_limiter_plant PROPERTIES FIXTURES_SETUP power_traces)
    add_test(NAME test_power_rule_windows_plant
        COMMAND ${CMAKE_COMMAND}
            -DPYTHON=${Python3_EXECUTABLE}
            -DCHECK=${repo_root}/tools/power_rule_windows.py
            -DDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/power_rule_windows_check.cmake)
    set_tests_properties(test_power_rule_windows_plant PROPERTIES FIXTURES_REQUIRED power_traces)

    # The VCU's Records (issue 61): the decoder on the files test_vcu_log
    # writes, the DBC decoding with them, and the rule-window check on the
    # decoded step logs. The DBC decoding needs cantools (requirements.txt).
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import cantools"
        RESULT_VARIABLE no_cantools OUTPUT_QUIET ERROR_QUIET)
    if(no_cantools AND TIM_REQUIRE_PYTHON)
        message(FATAL_ERROR "cantools is missing for test_data_log_vcu_decoder: pip install -r requirements.txt")
    elseif(no_cantools)
        message(WARNING "No cantools (pip install -r requirements.txt): test_data_log_vcu_decoder is not run")
    else()
        set_tests_properties(test_vcu_log PROPERTIES FIXTURES_SETUP vcu_log_sample)
        add_test(NAME test_data_log_vcu_decoder
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/data_log_vcu_check.py
                ${repo_root}/tools/data_log_decode.py
                ${repo_root}/tools/power_rule_windows.py
                ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(test_data_log_vcu_decoder PROPERTIES FIXTURES_REQUIRED vcu_log_sample)
    endif()
else()
    message(WARNING "No Python 3.10 or later: test_data_log_decoder and the power rule-window checks are not run")
endif()
