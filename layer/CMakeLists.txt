add_library(
    depthxr_core
    STATIC
    src/config_parser.cpp
    src/input_devices.cpp
    src/quadviews_sizing.cpp
    src/runtime_pacing.cpp
    src/runtime_relay.cpp
    src/runtime_compatibility.cpp
    src/seen_apps.cpp
    src/turbo_trace.cpp
    src/turbo_metrics.cpp
    src/turbo_recovery.cpp
    src/settings.cpp
    src/sound_player.cpp
    src/settings_resolver.cpp
    src/swapchain_state.cpp
    src/util/config_path.cpp
    src/util/logger.cpp
    src/util/process_info.cpp
    src/math/stereo_boost.cpp
    src/math/convergence.cpp
    src/math/depth_submission.cpp
    src/math/pivot_step.cpp
    src/math/pivot_view.cpp
    src/math/pivot_yaw.cpp
)

target_include_directories(depthxr_core PUBLIC include)
target_compile_features(depthxr_core PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(depthxr_core PUBLIC dinput8 dxguid ole32 winmm)
endif()

if(WIN32 AND DEPTHXR_BUILD_LAYER)
    find_package(OpenXR CONFIG QUIET)

    if(NOT OpenXR_FOUND)
        file(GLOB _openxr_nupkgs "${CMAKE_SOURCE_DIR}/OpenXR.Loader.*.nupkg")
        list(LENGTH _openxr_nupkgs _openxr_nupkg_count)

        if(_openxr_nupkg_count GREATER 0)
            list(GET _openxr_nupkgs 0 _openxr_nupkg)
            set(_openxr_extract_dir "${CMAKE_BINARY_DIR}/_deps/openxr_loader")

            file(MAKE_DIRECTORY "${_openxr_extract_dir}")
            file(ARCHIVE_EXTRACT INPUT "${_openxr_nupkg}" DESTINATION "${_openxr_extract_dir}")

            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_openxr_arch_dir "x64")
            else()
                set(_openxr_arch_dir "Win32")
            endif()

            set(_openxr_include_dir "${_openxr_extract_dir}/include")
            set(_openxr_implib "${_openxr_extract_dir}/native/${_openxr_arch_dir}/release/lib/openxr_loader.lib")
            set(_openxr_dll "${_openxr_extract_dir}/native/${_openxr_arch_dir}/release/bin/openxr_loader.dll")

            if(NOT EXISTS "${_openxr_implib}" OR NOT EXISTS "${_openxr_dll}")
                message(FATAL_ERROR "Vendored OpenXR loader package '${_openxr_nupkg}' is missing native/${_openxr_arch_dir}/release artifacts.")
            endif()

            add_library(OpenXR::openxr_loader SHARED IMPORTED)
            set_target_properties(
                OpenXR::openxr_loader
                PROPERTIES
                IMPORTED_IMPLIB "${_openxr_implib}"
                IMPORTED_LOCATION "${_openxr_dll}"
                INTERFACE_INCLUDE_DIRECTORIES "${_openxr_include_dir}"
            )

            set(OpenXR_FOUND TRUE)
            message(STATUS "Using vendored OpenXR loader from ${_openxr_nupkg}")
        else()
            message(FATAL_ERROR "OpenXR SDK not found. Install an OpenXR SDK that provides OpenXRConfig.cmake, or place an OpenXR.Loader.*.nupkg package in the repository root.")
        endif()
    endif()

    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/b379292b2ab6df5771ba9870d53cf8b2c9295daf.tar.gz
        URL_HASH SHA256=2ad177c0e468ac2eb3db9b5be6b8a18a6730d339378ac670e5fbda3503ac7d64
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(vulkan_headers)
    add_library(depthxr_osd STATIC src/osd_renderer.cpp src/osd_graphics.cpp src/osd_vulkan.cpp src/osd_composite.cpp)
    target_include_directories(depthxr_osd PUBLIC include)
    target_compile_features(depthxr_osd PUBLIC cxx_std_20)
    target_compile_definitions(depthxr_osd PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN)
    target_link_libraries(depthxr_osd PUBLIC depthxr_core OpenXR::openxr_loader Vulkan::Headers d3d11 d3d12 d3dcompiler gdi32)
    if(BUILD_TESTING)
        add_executable(depthxr_osd_composite_tests tests/osd_composite_tests.cpp)
        target_link_libraries(depthxr_osd_composite_tests PRIVATE depthxr_osd)
        target_compile_definitions(depthxr_osd_composite_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        add_test(NAME depthxr_osd_composite_tests COMMAND depthxr_osd_composite_tests)
    endif()

    add_library(
        XR_APILAYER_DIENERTECH_VECTORXR
        SHARED
        src/dll_main.cpp
        src/layer_entry.cpp
        src/openxr_layer.cpp
    )

    target_include_directories(XR_APILAYER_DIENERTECH_VECTORXR PRIVATE include)
    target_link_libraries(
        XR_APILAYER_DIENERTECH_VECTORXR
        PRIVATE
        depthxr_core
        depthxr_osd
        OpenXR::openxr_loader
        d3d11
        d3dcompiler
        dxgi
        shlwapi
    )
    # Derive the layer version from the single source of truth (tauri.conf.json) so
    # the layer logs the same version the app reports. Set-Version.ps1 keeps that
    # file current; CMake's project(VERSION) is a separate, unmanaged C++ version.
    set(_vectorxr_version "unknown")
    set(_vectorxr_tauri_conf "${CMAKE_SOURCE_DIR}/app/src-tauri/tauri.conf.json")
    if(EXISTS "${_vectorxr_tauri_conf}")
        file(READ "${_vectorxr_tauri_conf}" _vectorxr_tauri_contents)
        string(JSON _vectorxr_version_parsed ERROR_VARIABLE _vectorxr_version_error
               GET "${_vectorxr_tauri_contents}" version)
        if(_vectorxr_version_error)
            message(WARNING "Could not read version from ${_vectorxr_tauri_conf}: ${_vectorxr_version_error}")
        else()
            set(_vectorxr_version "${_vectorxr_version_parsed}")
        endif()
        # Re-run configure when the version bumps so the baked-in value never goes stale.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vectorxr_tauri_conf}")
    endif()
    message(STATUS "VectorXR layer version: ${_vectorxr_version}")

    target_compile_definitions(
        XR_APILAYER_DIENERTECH_VECTORXR
        PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        XR_USE_GRAPHICS_API_D3D11
        VECTORXR_VERSION="${_vectorxr_version}"
    )

    add_custom_command(
        TARGET XR_APILAYER_DIENERTECH_VECTORXR
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/manifest/XR_APILAYER_DIENERTECH_VECTORXR.json
                $<TARGET_FILE_DIR:XR_APILAYER_DIENERTECH_VECTORXR>/XR_APILAYER_DIENERTECH_VECTORXR.json
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:XR_APILAYER_DIENERTECH_VECTORXR>/sounds
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/activate.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/deactivate.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/origin-set.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/origin-release.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/turbo-on.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/turbo-off.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/metrics-on.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/metrics-off.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/depth-lock-on.wav
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/depth-lock-off.wav
                $<TARGET_FILE_DIR:XR_APILAYER_DIENERTECH_VECTORXR>/sounds/
    )

    if(DEPTHXR_BUILD_HARDWARE_PROBES)
        add_executable(
            vectorxr_hardware_probe
            tools/pivot_space_probe.cpp
        )
        target_link_libraries(
            vectorxr_hardware_probe
            PRIVATE
            OpenXR::openxr_loader
            d3d11
            d3dcompiler
            dxgi
        )
        target_compile_definitions(
            vectorxr_hardware_probe
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            XR_USE_PLATFORM_WIN32
            XR_USE_GRAPHICS_API_D3D11
        )
        if(MSVC)
            target_compile_options(vectorxr_hardware_probe PRIVATE /W4 /permissive-)
        endif()
        add_custom_command(
            TARGET vectorxr_hardware_probe
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:OpenXR::openxr_loader>
                    $<TARGET_FILE_DIR:vectorxr_hardware_probe>
        )

        if(DEPTHXR_BUILD_TESTS)
            add_test(
                NAME vectorxr_hardware_probe_self_test
                COMMAND vectorxr_hardware_probe --self-test
            )
            set_tests_properties(vectorxr_hardware_probe_self_test PROPERTIES TIMEOUT 10)
        endif()
    endif()

    if(DEPTHXR_BUILD_TESTS)
        add_executable(depthxr_osd_backend_tests tests/osd_backend_tests.cpp)
        target_link_libraries(depthxr_osd_backend_tests PRIVATE depthxr_osd dxgi)
        target_compile_definitions(depthxr_osd_backend_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN)
        add_test(NAME depthxr_osd_d3d12_tests COMMAND depthxr_osd_backend_tests d3d12)
        add_test(NAME depthxr_osd_vulkan_tests COMMAND depthxr_osd_backend_tests vulkan)
        set_tests_properties(depthxr_osd_d3d12_tests depthxr_osd_vulkan_tests PROPERTIES TIMEOUT 30)
        set_tests_properties(depthxr_osd_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(depthxr_osd_tests tests/osd_tests.cpp)
        target_link_libraries(depthxr_osd_tests PRIVATE depthxr_osd)
        target_compile_definitions(depthxr_osd_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11)
        add_test(NAME depthxr_osd_tests COMMAND depthxr_osd_tests)
        set_tests_properties(depthxr_osd_tests PROPERTIES TIMEOUT 20)

        # Compile the real frame-loop implementation into a dedicated fake-runtime
        # harness. DEPTHXR_TESTING only grants the harness state setup/inspection
        # access; no test hooks are present in the shipping layer target.
        add_executable(
            depthxr_turbo_frame_tests
            tests/turbo_frame_tests.cpp
            src/openxr_layer.cpp
        )
        target_include_directories(depthxr_turbo_frame_tests PRIVATE include)
        target_link_libraries(
            depthxr_turbo_frame_tests
            PRIVATE
            depthxr_core
            depthxr_osd
            OpenXR::openxr_loader
            d3d11
            d3dcompiler
            dxgi
            shlwapi
        )
        target_compile_definitions(
            depthxr_turbo_frame_tests
            PRIVATE
            DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            XR_USE_GRAPHICS_API_D3D11
            VECTORXR_VERSION="${_vectorxr_version}"
        )
        add_test(NAME depthxr_turbo_frame_tests COMMAND depthxr_turbo_frame_tests)
        set_tests_properties(depthxr_turbo_frame_tests PROPERTIES TIMEOUT 15)

        add_executable(depthxr_pivot_pose_tests tests/pivot_pose_tests.cpp src/openxr_layer.cpp)
        target_include_directories(depthxr_pivot_pose_tests PRIVATE include)
        target_link_libraries(depthxr_pivot_pose_tests PRIVATE depthxr_core depthxr_osd
            OpenXR::openxr_loader d3d11 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_pivot_pose_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}")
        add_test(NAME depthxr_pivot_pose_tests COMMAND depthxr_pivot_pose_tests)
        set_tests_properties(depthxr_pivot_pose_tests PROPERTIES TIMEOUT 15)
    endif()
endif()

if(DEPTHXR_BUILD_TESTS)
    add_executable(
        depthxr_layer_tests
        tests/config_tests.cpp
    )

    target_link_libraries(depthxr_layer_tests PRIVATE depthxr_core)
    if(WIN32)
        target_link_libraries(depthxr_layer_tests PRIVATE d3dcompiler)
        target_compile_definitions(
            depthxr_layer_tests
            PRIVATE VECTORXR_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
    endif()
    add_test(NAME depthxr_layer_tests COMMAND depthxr_layer_tests)
    add_executable(depthxr_turbo_recovery_tests tests/turbo_recovery_tests.cpp)
    target_link_libraries(depthxr_turbo_recovery_tests PRIVATE depthxr_core)
    add_test(NAME depthxr_turbo_recovery_tests COMMAND depthxr_turbo_recovery_tests)
    set_tests_properties(depthxr_turbo_recovery_tests PROPERTIES TIMEOUT 10)

    add_executable(
        depthxr_action_set_attachment_tests
        tests/action_set_attachment_tests.cpp
    )
    target_include_directories(depthxr_action_set_attachment_tests PRIVATE include)
    add_test(NAME depthxr_action_set_attachment_tests COMMAND depthxr_action_set_attachment_tests)
endif()
