add_library(
    depthxr_core
    STATIC
    src/config_parser.cpp
    src/settings.cpp
    src/settings_resolver.cpp
    src/util/config_path.cpp
    src/util/logger.cpp
    src/util/process_info.cpp
    src/math/stereo_boost.cpp
    src/math/world_scale.cpp
    src/math/fov_scale.cpp
)

target_include_directories(depthxr_core PUBLIC include)
target_compile_features(depthxr_core PUBLIC cxx_std_20)

if(WIN32 AND DEPTHXR_BUILD_LAYER)
    find_package(OpenXR CONFIG REQUIRED)

    add_library(
        XR_APILAYER_NOVENDOR_depthxr
        SHARED
        src/dll_main.cpp
        src/layer_entry.cpp
        src/openxr_layer.cpp
    )

    target_include_directories(XR_APILAYER_NOVENDOR_depthxr PRIVATE include)
    target_link_libraries(
        XR_APILAYER_NOVENDOR_depthxr
        PRIVATE
        depthxr_core
        OpenXR::openxr_loader
        shlwapi
    )
    target_compile_definitions(XR_APILAYER_NOVENDOR_depthxr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

    add_custom_command(
        TARGET XR_APILAYER_NOVENDOR_depthxr
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/manifest/XR_APILAYER_NOVENDOR_depthxr.json
                $<TARGET_FILE_DIR:XR_APILAYER_NOVENDOR_depthxr>/XR_APILAYER_NOVENDOR_depthxr.json
    )
endif()

if(DEPTHXR_BUILD_TESTS)
    add_executable(
        depthxr_layer_tests
        tests/config_tests.cpp
    )

    target_link_libraries(depthxr_layer_tests PRIVATE depthxr_core)
    add_test(NAME depthxr_layer_tests COMMAND depthxr_layer_tests)
endif()
