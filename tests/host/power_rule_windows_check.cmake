# Judges every trace test_vcu_power_limiter_plant wrote with
# tools/power_rule_windows.py, the one check of the 80 kW rule's two windows
# (issue 60: "judges the host test output"; EV.3.3.1, EV.3.4.1).
#
#     cmake -DPYTHON=<python> -DCHECK=<check> -DDIR=<test dir> -P <this file>

file(GLOB traces ${DIR}/power_limiter_*.csv)
list(LENGTH traces count)
if(count LESS 4)
    message(FATAL_ERROR "expected 4 plant traces in ${DIR}, found ${count}")
endif()

set(failed "")
foreach(trace ${traces})
    execute_process(COMMAND ${PYTHON} ${CHECK} ${trace} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        list(APPEND failed ${trace})
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "a rule window is broken in: ${failed}")
endif()
