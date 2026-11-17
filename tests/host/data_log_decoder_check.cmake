# Runs tools/data_log_decode.py on the sample file test_data_log_record wrote
# and compares what it makes of it with what that test says it must (issue
# 53: "a host test checks it against the Python decoder").
#
#     cmake -DPYTHON=<python> -DDECODER=<decoder> -DDIR=<test dir> -P <this file>

set(sample ${DIR}/data_log_sample.tdl)
set(out ${DIR}/data_log_actual)

file(REMOVE_RECURSE ${out})
execute_process(
    COMMAND ${PYTHON} ${DECODER} ${sample} -o ${out} --summary ${out}/data_log_sample.summary.txt
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the decoder failed: ${result}")
endif()

foreach(part step.csv event.csv summary.txt)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files
            ${DIR}/data_log_expected.${part} ${out}/data_log_sample.${part}
        RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
        file(READ ${DIR}/data_log_expected.${part} want)
        file(READ ${out}/data_log_sample.${part} got)
        message(FATAL_ERROR "${part} differs.\n--- expected\n${want}--- decoder\n${got}")
    endif()
endforeach()
