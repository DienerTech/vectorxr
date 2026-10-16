# The layer DLL carries its own C and C++ runtime (/MT): it must not import msvcp140, vcruntime140 or the Universal
# C Runtime. A game's msvcp140 older than 14.38 crashes the layer on its first lock, and without the VC++
# redistributable the layer cannot load at all, which fails xrCreateInstance in every OpenXR app.
#
#   cmake -DDUMPBIN=<dumpbin.exe> -DLAYER=<XR_APILAYER_DIENERTECH_VECTORXR.dll> -P layer_crt_imports.cmake
if(NOT DUMPBIN OR NOT LAYER)
    message(FATAL_ERROR "DUMPBIN and LAYER are required")
endif()
execute_process(
    COMMAND "${DUMPBIN}" /nologo /dependents "${LAYER}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE dependents
    ERROR_VARIABLE errors
    TIMEOUT 60)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dumpbin /dependents exited ${result}\n${errors}")
endif()
string(TOUPPER "${dependents}" dependents_upper)
if(NOT dependents_upper MATCHES "KERNEL32\\.DLL")
    message(FATAL_ERROR "dumpbin listed no dependencies of ${LAYER}:\n${dependents}")
endif()
string(REGEX MATCHALL "[^ \t\r\n]+\\.DLL" dlls "${dependents_upper}")
set(crt_dlls "")
foreach(dll IN LISTS dlls)
    if(dll MATCHES "^(MSVCP|VCRUNTIME|MSVCR|CONCRT|VCOMP|UCRTBASE|API-MS-WIN-CRT-)")
        list(APPEND crt_dlls "${dll}")
    endif()
endforeach()
if(crt_dlls)
    list(JOIN crt_dlls ", " crt_dlls)
    message(FATAL_ERROR "The layer imports the C or C++ runtime (${crt_dlls}): link it statically "
                        "(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)")
endif()
message(STATUS "The layer carries its own C and C++ runtime")
