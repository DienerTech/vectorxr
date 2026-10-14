# The layer DLL with the C++ runtime as a DLL: it must initialize its mutexes and condition variables in msvcp140
# (_Mtx_init_in_situ, _Cnd_init_in_situ), as a game's msvcp140 older than 14.38 cannot lock one built inline.
#
#   cmake -DDUMPBIN=<dumpbin.exe> -DLAYER=<XR_APILAYER_DIENERTECH_VECTORXR.dll> -P layer_crt_imports.cmake
if(NOT DUMPBIN OR NOT LAYER)
    message(FATAL_ERROR "DUMPBIN and LAYER are required")
endif()
execute_process(
    COMMAND "${DUMPBIN}" /nologo /imports "${LAYER}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE imports
    ERROR_VARIABLE errors
    TIMEOUT 60)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dumpbin /imports exited ${result}\n${errors}")
endif()
string(TOUPPER "${imports}" imports_upper)
if(NOT imports_upper MATCHES "MSVCP140")
    message(STATUS "The layer does not import msvcp140")
    return()
endif()
foreach(kind Mtx Cnd)
    if(imports MATCHES "_${kind}_[a-z]" AND NOT imports MATCHES "_${kind}_init_in_situ")
        message(FATAL_ERROR "The layer calls msvcp140's _${kind}_ functions without _${kind}_init_in_situ: "
                            "build it with _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
    endif()
endforeach()
message(STATUS "The layer initializes its mutexes and condition variables in msvcp140")
