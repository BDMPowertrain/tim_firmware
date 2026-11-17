# The build ID (spec #36, ADR 0016): the commit an image is built from, so
# that the boot Event says which image ran and a later pedal check can tell
# a new flash from the old one.
#
# tim_build_id(<target>) gives <target> a generated header, build_id.h:
#   BUILD_ID     the first 8 hex digits of HEAD as a uint32_t, 0 if git
#                or the repository is not there
#   BUILD_DIRTY  1 if a tracked file differs from HEAD
# It is rewritten on every build but only touched when it changes, so a
# build of the same tree recompiles nothing.

include_guard(GLOBAL)

set(TIM_BUILD_ID_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/build_id_write.cmake)

function(tim_build_id target)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/build_id)
    set(header ${dir}/build_id.h)
    get_filename_component(repo ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)

    add_custom_target(${target}_build_id
        COMMAND ${CMAKE_COMMAND} -DREPO=${repo} -DOUT=${header} -P ${TIM_BUILD_ID_SCRIPT}
        BYPRODUCTS ${header}
        COMMENT "Build ID"
        VERBATIM)
    add_dependencies(${target} ${target}_build_id)
    target_include_directories(${target} PRIVATE ${dir})
endfunction()
