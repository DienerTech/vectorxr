# The hardware probe's --render-check on a machine that lacks what it needs: run with the Vulkan validation layer out
# of the loader's reach, it must exit 77 (skipped), and 1 (failed) with VECTORXR_TESTS_REQUIRE_HW=1.
#
#   cmake -DPROBE=<vectorxr_hardware_probe.exe> -DEMPTY_LAYER_DIR=<an empty folder> -P probe_expect_unavailable.cmake
if(NOT PROBE OR NOT EMPTY_LAYER_DIR)
    message(FATAL_ERROR "PROBE and EMPTY_LAYER_DIR are required")
endif()
file(MAKE_DIRECTORY "${EMPTY_LAYER_DIR}")
# VK_LAYER_PATH replaces the loader's explicit-layer search (the registry's included), VK_ADD_LAYER_PATH is where the
# probe would otherwise find the build's own copy.
set(ENV{VK_LAYER_PATH} "${EMPTY_LAYER_DIR}")
set(ENV{VK_ADD_LAYER_PATH} "${EMPTY_LAYER_DIR}")

foreach(require OFF ON)
    if(require)
        set(ENV{VECTORXR_TESTS_REQUIRE_HW} "1")
        set(expected 1)
    else()
        unset(ENV{VECTORXR_TESTS_REQUIRE_HW})
        set(expected 77)
    endif()
    execute_process(
        COMMAND "${PROBE}" --render-check --graphics vulkan --vulkan-validation
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 60)
    message(STATUS "VECTORXR_TESTS_REQUIRE_HW=${require}: exit ${result}\n${output}${errors}")
    if(NOT result STREQUAL expected)
        message(FATAL_ERROR "--render-check without the validation layer exited ${result}, expected ${expected}")
    endif()
    if(NOT errors MATCHES "render check (skipped|FAILED)")
        message(FATAL_ERROR "--render-check did not say why it could not run")
    endif()
endforeach()
