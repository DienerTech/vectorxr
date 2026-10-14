add_library(
    depthxr_core
    STATIC
    src/config_parser.cpp
    src/input_devices.cpp
    src/quadviews_compose.cpp
    src/quadviews_sizing.cpp
    src/framegen_plan.cpp
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
    src/session_graphics.cpp
    src/swapchain_state.cpp
    src/thread_census.cpp
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
    add_library(depthxr_osd STATIC src/osd_renderer.cpp src/osd_graphics.cpp src/osd_vulkan.cpp)
    target_include_directories(depthxr_osd PUBLIC include)
    target_compile_features(depthxr_osd PUBLIC cxx_std_20)
    target_compile_definitions(depthxr_osd PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN)
    target_link_libraries(depthxr_osd PUBLIC depthxr_core OpenXR::openxr_loader Vulkan::Headers d3d11 d3d12 d3dcompiler gdi32)

    # Frame Generation compute shaders. One HLSL source compiles to DXBC for
    # Direct3D 11 and 12 (fxc from the Windows SDK) and to SPIR-V for Vulkan.
    # The Windows SDK's dxc is built without SPIR-V code generation, so a
    # pinned DXC release is fetched the same way as the Vulkan headers.
    option(DEPTHXR_FRAMEGEN_SPIRV "Compile Frame Generation SPIR-V for Vulkan with a pinned DXC release" ON)
    cmake_host_system_information(RESULT _framegen_kits_root QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE "KitsRoot10")
    set(_framegen_fxc_hints "")
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        list(APPEND _framegen_fxc_hints "${_framegen_kits_root}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
    endif()
    file(GLOB _framegen_kit_bins LIST_DIRECTORIES true "${_framegen_kits_root}/bin/10.*")
    list(SORT _framegen_kit_bins COMPARE NATURAL ORDER DESCENDING)
    foreach(_framegen_kit_bin IN LISTS _framegen_kit_bins)
        list(APPEND _framegen_fxc_hints "${_framegen_kit_bin}/x64")
    endforeach()
    find_program(DEPTHXR_FXC fxc HINTS ${_framegen_fxc_hints} REQUIRED)
    set(DEPTHXR_DXC "")
    if(DEPTHXR_FRAMEGEN_SPIRV)
        FetchContent_Declare(dxc_release
            URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip
            URL_HASH SHA256=9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(dxc_release)
        set(DEPTHXR_DXC "${dxc_release_SOURCE_DIR}/bin/x64/dxc.exe")
    endif()
    set(_framegen_passes LUMA DOWNSAMPLE SEARCH REFINE FILTER SYNTHESIZE_UNORM8 SYNTHESIZE_FLOAT16 DEPTH DEPTH_CHECK DEPTH_TRUST FLOW_PACK FLOW_UNPACK
        TRUST_UPDATE SHIFT_CLEAR SHIFT_SPLAT SHIFT_RESOLVE SHIFT_FILL FLOW_PACK_RAW TRUST_RESOLVE SHADING_RATE WORLD_REDUCE WORLD_SOLVE
        TRUST_VOTE DEPTH_SCALE DEPTH_SCALE_VOTE DEPTH_STALE)
    set(_framegen_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/framegen.hlsl")
    set(_framegen_out "${CMAKE_CURRENT_BINARY_DIR}/framegen_shaders")
    file(MAKE_DIRECTORY "${_framegen_out}")
    set(_framegen_binaries "")
    foreach(_framegen_pass IN LISTS _framegen_passes)
        add_custom_command(
            OUTPUT "${_framegen_out}/${_framegen_pass}.dxbc"
            COMMAND "${DEPTHXR_FXC}" -nologo -T cs_5_0 -E CSMain -D "FG_PASS_${_framegen_pass}=1" -O3 -WX
                    -Fo "${_framegen_out}/${_framegen_pass}.dxbc" "${_framegen_hlsl}"
            DEPENDS "${_framegen_hlsl}"
            COMMENT "Compiling Frame Generation ${_framegen_pass} (DXBC)"
            VERBATIM)
        list(APPEND _framegen_binaries "${_framegen_out}/${_framegen_pass}.dxbc")
        if(DEPTHXR_DXC)
            add_custom_command(
                OUTPUT "${_framegen_out}/${_framegen_pass}.spv"
                COMMAND "${DEPTHXR_DXC}" -nologo -spirv -T cs_6_0 -E CSMain -D "FG_PASS_${_framegen_pass}=1" -O3 -WX
                        -fspv-target-env=vulkan1.0 -Fo "${_framegen_out}/${_framegen_pass}.spv" "${_framegen_hlsl}"
                DEPENDS "${_framegen_hlsl}"
                COMMENT "Compiling Frame Generation ${_framegen_pass} (SPIR-V)"
                VERBATIM)
            list(APPEND _framegen_binaries "${_framegen_out}/${_framegen_pass}.spv")
        endif()
    endforeach()
    # Synthesis as a draw into the output swapchains (Direct3D 12): the
    # vertex shader and a pixel shader per number of generated frames; then
    # the quad views compose pixel shader, drawn by the same vertex shader.
    set(_framegen_draw_shaders DRAW_VS DRAW_PS1 DRAW_PS2 DRAW_PS3 DRAW_PS4 COMPOSE_PS)
    foreach(_framegen_draw IN LISTS _framegen_draw_shaders)
        if(_framegen_draw STREQUAL "DRAW_VS")
            set(_framegen_draw_args -T vs_5_0 -E VSMain -D FG_DRAW_OUTPUTS=1 -D FG_PASS_SYNTHESIZE_DRAW=1)
        elseif(_framegen_draw STREQUAL "COMPOSE_PS")
            set(_framegen_draw_args -T ps_5_0 -E PSMain -D FG_PASS_COMPOSE_DRAW=1)
        else()
            string(SUBSTRING "${_framegen_draw}" 7 1 _framegen_draw_outputs)
            set(_framegen_draw_args -T ps_5_0 -E PSMain -D "FG_DRAW_OUTPUTS=${_framegen_draw_outputs}"
                -D FG_PASS_SYNTHESIZE_DRAW=1)
        endif()
        add_custom_command(
            OUTPUT "${_framegen_out}/${_framegen_draw}.dxbc"
            COMMAND "${DEPTHXR_FXC}" -nologo ${_framegen_draw_args} -O3 -WX
                    -Fo "${_framegen_out}/${_framegen_draw}.dxbc" "${_framegen_hlsl}"
            DEPENDS "${_framegen_hlsl}"
            COMMENT "Compiling Frame Generation ${_framegen_draw} (DXBC)"
            VERBATIM)
        list(APPEND _framegen_binaries "${_framegen_out}/${_framegen_draw}.dxbc")
    endforeach()
    # Quad views' flow merge (Direct3D 12): a compute shader outside the
    # FrameGenPass list, included by the backend as a header.
    add_custom_command(
        OUTPUT "${_framegen_out}/framegen_flow_merge.h"
        COMMAND "${DEPTHXR_FXC}" -nologo -T cs_5_0 -E CSMain -D FG_PASS_FLOW_MERGE=1 -O3 -WX
                -Vn kFrameGenFlowMergeDxbc -Fh "${_framegen_out}/framegen_flow_merge.h" "${_framegen_hlsl}"
        DEPENDS "${_framegen_hlsl}"
        COMMENT "Compiling Frame Generation FLOW_MERGE (DXBC)"
        VERBATIM)
    string(REPLACE ";" "," _framegen_pass_list "${_framegen_passes}")
    string(REPLACE ";" "," _framegen_draw_list "${_framegen_draw_shaders}")
    set(_framegen_spirv_dir "")
    if(DEPTHXR_DXC)
        set(_framegen_spirv_dir "${_framegen_out}")
    endif()
    add_custom_command(
        OUTPUT "${_framegen_out}/framegen_shaders.cpp"
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${_framegen_out}/framegen_shaders.cpp" "-DPASSES=${_framegen_pass_list}"
                "-DDRAW=${_framegen_draw_list}" "-DDXBC_DIR=${_framegen_out}" "-DSPIRV_DIR=${_framegen_spirv_dir}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFrameGenShaders.cmake"
        DEPENDS ${_framegen_binaries} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFrameGenShaders.cmake"
        COMMENT "Embedding Frame Generation shaders"
        VERBATIM)
    add_library(depthxr_framegen STATIC
        src/framegen/framegen_gpu.cpp
        src/framegen/framegen_presenter.cpp
        src/framegen/framegen_runtime_queue.cpp
        src/framegen/framegen_trace.cpp
        src/framegen/framegen_d3d11_bridge.cpp
        src/framegen/framegen_vulkan_bridge.cpp
        src/framegen/framegen_vulkan_formats.cpp
        src/framegen/vulkan_enable_hooks.cpp
        src/framegen/framegen_d3d11.cpp
        src/framegen/framegen_d3d12.cpp
        src/framegen/framegen_vulkan.cpp
        # Not Frame Generation's: the session's adapters for its log line,
        # which needs the Direct3D 12 and Vulkan types the layer lacks.
        src/session_graphics_query.cpp
        "${_framegen_out}/framegen_shaders.cpp"
        "${_framegen_out}/framegen_flow_merge.h")
    target_include_directories(depthxr_framegen PUBLIC include)
    target_include_directories(depthxr_framegen PRIVATE "${_framegen_out}")
    target_compile_features(depthxr_framegen PUBLIC cxx_std_20)
    # NVIDIA's optical flow accelerator (RTX 20-50) measures motion on
    # Direct3D 12, through the SDK's MIT-licensed interface headers in
    # third_party/nvidia-optical-flow; nvofapi64.dll ships with the driver
    # and is loaded at run time.
    set(DEPTHXR_NVOF_INCLUDE "${PROJECT_SOURCE_DIR}/third_party/nvidia-optical-flow")
    target_include_directories(depthxr_framegen PRIVATE "${DEPTHXR_NVOF_INCLUDE}")
    target_compile_definitions(depthxr_framegen PRIVATE DEPTHXR_NVOF)
    target_compile_definitions(depthxr_framegen PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN)
    target_link_libraries(depthxr_framegen PUBLIC depthxr_core OpenXR::openxr_loader Vulkan::Headers d3d11 d3d12 dxgi)

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
        depthxr_framegen
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

    # On a machine that has the GPU and the validation layer, the Vulkan
    # bridge's suites (and the hardware probe's render checks) must run
    # validated: a skip (77) then fails them.
    option(DEPTHXR_REQUIRE_HW_TESTS "The Vulkan bridge tests fail instead of skipping without a GPU or validation" OFF)

    if(DEPTHXR_BUILD_HARDWARE_PROBES)
        add_executable(
            vectorxr_hardware_probe
            tools/pivot_space_probe.cpp
            tools/probe_gpu_load.cpp
            tools/probe_d3d11.cpp
            tools/probe_vulkan.cpp
            tools/probe_render_check.cpp
            tools/probe_vram_ballast.cpp
            tools/probe_common.h
            tests/test_adapter.h
        )
        # FG_TEST_ADAPTER's helper is shared with the GPU tests.
        target_include_directories(vectorxr_hardware_probe PRIVATE tests)
        # Vulkan is loaded at run time (vulkan-1.dll): only its headers are linked.
        target_link_libraries(
            vectorxr_hardware_probe
            PRIVATE
            OpenXR::openxr_loader
            Vulkan::Headers
            d3d11
            d3d12
            d3dcompiler
            dxgi
        )
        target_compile_features(vectorxr_hardware_probe PRIVATE cxx_std_20)
        target_compile_definitions(
            vectorxr_hardware_probe
            PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            XR_USE_PLATFORM_WIN32
            XR_USE_GRAPHICS_API_D3D11
            XR_USE_GRAPHICS_API_VULKAN
            VK_NO_PROTOTYPES
        )
        if(MSVC)
            target_compile_options(vectorxr_hardware_probe PRIVATE /W4 /permissive-)
        endif()
        # --graphics vulkan --gpu-ms: the GPU load as SPIR-V from the pinned DXC (without it the Vulkan mode runs
        # without --gpu-ms).
        if(DEPTHXR_DXC)
            set(_probe_shaders "${CMAKE_CURRENT_BINARY_DIR}/probe_shaders")
            file(MAKE_DIRECTORY "${_probe_shaders}")
            add_custom_command(
                OUTPUT "${_probe_shaders}/probe_vulkan_load.h"
                COMMAND "${DEPTHXR_DXC}" -nologo -spirv -T cs_6_0 -E CSMain -O3 -WX -fspv-target-env=vulkan1.0
                        -Vn kProbeLoadSpirv -Fh "${_probe_shaders}/probe_vulkan_load.h"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_vulkan_load.hlsl"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_vulkan_load.hlsl"
                COMMENT "Compiling the hardware probe's Vulkan GPU load (SPIR-V)"
                VERBATIM)
            target_sources(vectorxr_hardware_probe PRIVATE "${_probe_shaders}/probe_vulkan_load.h")
            target_include_directories(vectorxr_hardware_probe PRIVATE "${_probe_shaders}")
            target_compile_definitions(vectorxr_hardware_probe PRIVATE VECTORXR_PROBE_SPIRV=1)
        endif()
        # --vulkan-validation without VK_ADD_LAYER_PATH: the validation layer manifest the tests' configuration
        # writes into this build tree, when there is one.
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/vulkan_layers" _probe_vk_layer_dir)
        string(REPLACE "\\" "\\\\" _probe_vk_layer_dir "${_probe_vk_layer_dir}")
        target_compile_definitions(vectorxr_hardware_probe PRIVATE
            "VECTORXR_PROBE_VULKAN_LAYER_DIR=\"${_probe_vk_layer_dir}\"")
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
            # The rendering half on this machine's GPU, without OpenXR: Direct3D 11 with its debug layer, Vulkan
            # validated (with synchronization validation) when the tests have a validation layer.
            set(_probe_validation "")
            if(DEPTHXR_VULKAN_VALIDATION_LAYER OR
               (DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/VkLayer_khronos_validation.dll"))
                set(_probe_validation --vulkan-validation)
            endif()
            add_test(NAME vectorxr_hardware_probe_render_check_d3d11
                     COMMAND vectorxr_hardware_probe --render-check --d3d11-debug)
            add_test(NAME vectorxr_hardware_probe_render_check_vulkan
                     COMMAND vectorxr_hardware_probe --render-check --graphics vulkan ${_probe_validation})
            add_test(NAME vectorxr_hardware_probe_render_check_vulkan_late_1_0
                     COMMAND vectorxr_hardware_probe --render-check --graphics vulkan --vk-late-submit
                             --vulkan-api 1.0 ${_probe_validation})
            # The checks of the checks: a VkFence left undestroyed must fail the render check (validation reports it
            # at vkDestroyDevice, before the summary), and a queue that never goes idle must end the run within its
            # 10 s waits instead of hanging (then the check releases it, so no process exits with a stalled queue).
            set(_probe_render_checks vectorxr_hardware_probe_render_check_d3d11
                vectorxr_hardware_probe_render_check_vulkan vectorxr_hardware_probe_render_check_vulkan_late_1_0
                vectorxr_hardware_probe_render_check_vulkan_stuck_queue)
            add_test(NAME vectorxr_hardware_probe_render_check_vulkan_stuck_queue
                     COMMAND vectorxr_hardware_probe --render-check --graphics vulkan --vk-test-stuck-queue)
            string(CONCAT _probe_stuck_output "did not finish within 10 s before the swapchains are destroyed"
                   ".*the queue ran on.*did not finish on the GPU within 10 s")
            set_tests_properties(vectorxr_hardware_probe_render_check_vulkan_stuck_queue PROPERTIES
                                 PASS_REGULAR_EXPRESSION "${_probe_stuck_output}")
            if(_probe_validation)
                add_test(NAME vectorxr_hardware_probe_render_check_vulkan_leak
                         COMMAND vectorxr_hardware_probe --render-check --graphics vulkan --vulkan-validation
                                 --vk-test-leak)
                string(CONCAT _probe_leak_output "vkDestroyDevice-device-05137.*VkFence"
                       ".*0 failed, 1 debug/validation messages: FAILED")
                set_tests_properties(vectorxr_hardware_probe_render_check_vulkan_leak PROPERTIES
                                     PASS_REGULAR_EXPRESSION "${_probe_leak_output}")
                list(APPEND _probe_render_checks vectorxr_hardware_probe_render_check_vulkan_leak)
            endif()
            # A machine without vulkan-1.dll, a hardware GPU, the Direct3D 11 SDK layers or the validation layer
            # skips them (77), unless the tests require the hardware.
            set_tests_properties(${_probe_render_checks} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
            if(DEPTHXR_REQUIRE_HW_TESTS)
                set_tests_properties(${_probe_render_checks} PROPERTIES ENVIRONMENT "VECTORXR_TESTS_REQUIRE_HW=1")
            endif()
            add_test(NAME vectorxr_hardware_probe_render_check_unavailable
                     COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:vectorxr_hardware_probe>
                             -DEMPTY_LAYER_DIR=${CMAKE_CURRENT_BINARY_DIR}/probe_no_vulkan_layers
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_expect_unavailable.cmake)
            set_tests_properties(vectorxr_hardware_probe_render_check_unavailable PROPERTIES TIMEOUT 120)
        endif()
    endif()

    if(DEPTHXR_BUILD_TESTS)
        # The layer DLL's msvcp140 imports (dumpbin sits beside the linker), without a GPU.
        get_filename_component(_layer_linker_dir "${CMAKE_LINKER}" DIRECTORY)
        find_program(DEPTHXR_DUMPBIN dumpbin HINTS "${_layer_linker_dir}")
        if(DEPTHXR_DUMPBIN)
            add_test(NAME depthxr_layer_crt_imports_tests
                     COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${DEPTHXR_DUMPBIN}"
                             -DLAYER=$<TARGET_FILE:XR_APILAYER_DIENERTECH_VECTORXR>
                             -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_crt_imports.cmake")
            set_tests_properties(depthxr_layer_crt_imports_tests PROPERTIES TIMEOUT 60)
        endif()
        # FG_TEST_ADAPTER's name matching and environment parsing (test_adapter.h), without a GPU.
        add_executable(depthxr_test_adapter_tests tests/test_adapter_tests.cpp)
        target_compile_definitions(depthxr_test_adapter_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        add_test(NAME depthxr_test_adapter_tests COMMAND depthxr_test_adapter_tests)
        set_tests_properties(depthxr_test_adapter_tests PROPERTIES TIMEOUT 10)
        add_executable(depthxr_osd_backend_tests tests/osd_backend_tests.cpp)
        target_link_libraries(depthxr_osd_backend_tests PRIVATE depthxr_osd dxgi)
        target_compile_definitions(depthxr_osd_backend_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN)
        add_test(NAME depthxr_osd_d3d12_tests COMMAND depthxr_osd_backend_tests d3d12)
        add_test(NAME depthxr_osd_vulkan_tests COMMAND depthxr_osd_backend_tests vulkan)
        set_tests_properties(depthxr_osd_d3d12_tests depthxr_osd_vulkan_tests PROPERTIES TIMEOUT 30)
        set_tests_properties(depthxr_osd_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(depthxr_framegen_tests tests/framegen_backend_tests.cpp)
        # d3dcompiler: the quad views compose is checked against the layer's
        # Direct3D 11 compositor shader, compiled at run time.
        target_link_libraries(depthxr_framegen_tests PRIVATE depthxr_framegen dxgi d3dcompiler)
        target_compile_definitions(depthxr_framegen_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN)
        # Research: compares NVIDIA's hardware optical flow with ours
        # (`depthxr_framegen_tests nvof`).
        target_include_directories(depthxr_framegen_tests PRIVATE "${DEPTHXR_NVOF_INCLUDE}")
        target_compile_definitions(depthxr_framegen_tests PRIVATE DEPTHXR_NVOF_RESEARCH)
        add_test(NAME depthxr_framegen_d3d11_tests COMMAND depthxr_framegen_tests d3d11)
        add_test(NAME depthxr_framegen_d3d12_tests COMMAND depthxr_framegen_tests d3d12)
        add_test(NAME depthxr_framegen_vulkan_tests COMMAND depthxr_framegen_tests vulkan)
        set_tests_properties(depthxr_framegen_d3d11_tests depthxr_framegen_vulkan_tests PROPERTIES TIMEOUT 60)
        # WARP runs on the CPU: slower while other builds share it.
        set_tests_properties(depthxr_framegen_d3d12_tests PROPERTIES TIMEOUT 120)
        set_tests_properties(depthxr_framegen_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77)
        # The Direct3D 12 scenarios on the GPU: variable-rate shading, the optical flow hardware (a
        # session per view, four for quad views) and a GPU's fence budgets, none of which WARP has.
        # Passes with a message without a hardware adapter (and without the optical flow hardware,
        # its checks are skipped with one).
        add_test(NAME depthxr_framegen_d3d12_hw_tests COMMAND depthxr_framegen_tests hw-d3d12)
        set_tests_properties(depthxr_framegen_d3d12_hw_tests PROPERTIES TIMEOUT 180)
        # Direct3D 11 <-> 12 sharing probe for the D3D11 bridge. It needs a real GPU, so it is
        # built here but is not a CTest (`depthxr_bridge_probe --help`).
        add_executable(depthxr_bridge_probe tests/bridge_probe.cpp)
        target_link_libraries(depthxr_bridge_probe PRIVATE d3d11 d3d12 dxgi dxguid d3dcompiler)
        target_compile_definitions(depthxr_bridge_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_features(depthxr_bridge_probe PRIVATE cxx_std_20)
        # Frame Generation's Direct3D 11 bridge against a stand-in runtime, on WARP
        # (`depthxr_d3d11_bridge_tests hw` runs it on the GPU).
        add_executable(depthxr_d3d11_bridge_tests tests/d3d11_bridge_tests.cpp)
        target_link_libraries(depthxr_d3d11_bridge_tests PRIVATE depthxr_framegen d3d11 d3d12 dxgi)
        target_compile_definitions(depthxr_d3d11_bridge_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12)
        add_test(NAME depthxr_d3d11_bridge_tests COMMAND depthxr_d3d11_bridge_tests)
        add_test(NAME depthxr_d3d11_bridge_hw_tests COMMAND depthxr_d3d11_bridge_tests hw)
        set_tests_properties(depthxr_d3d11_bridge_tests depthxr_d3d11_bridge_hw_tests PROPERTIES TIMEOUT 120
                             SKIP_RETURN_CODE 77)
        add_executable(depthxr_framegen_presenter_tests tests/framegen_presenter_tests.cpp)
        target_link_libraries(depthxr_framegen_presenter_tests PRIVATE depthxr_framegen)
        target_compile_definitions(depthxr_framegen_presenter_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        add_test(NAME depthxr_framegen_presenter_tests COMMAND depthxr_framegen_presenter_tests)
        # Paced by the wall clock: about 200 s on a fast desktop, longer on a shared CI runner.
        set_tests_properties(depthxr_framegen_presenter_tests PROPERTIES TIMEOUT 480)
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
            depthxr_framegen
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
        set_tests_properties(depthxr_turbo_frame_tests PROPERTIES TIMEOUT 90)

        add_executable(depthxr_pivot_pose_tests tests/pivot_pose_tests.cpp src/openxr_layer.cpp)
        target_include_directories(depthxr_pivot_pose_tests PRIVATE include)
        target_link_libraries(depthxr_pivot_pose_tests PRIVATE depthxr_core depthxr_osd depthxr_framegen
            OpenXR::openxr_loader d3d11 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_pivot_pose_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}")
        add_test(NAME depthxr_pivot_pose_tests COMMAND depthxr_pivot_pose_tests)
        set_tests_properties(depthxr_pivot_pose_tests PROPERTIES TIMEOUT 15)

        add_executable(depthxr_pivot_snap_tests tests/pivot_snap_tests.cpp src/openxr_layer.cpp)
        target_include_directories(depthxr_pivot_snap_tests PRIVATE include)
        target_link_libraries(depthxr_pivot_snap_tests PRIVATE depthxr_core depthxr_osd depthxr_framegen
            OpenXR::openxr_loader d3d11 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_pivot_snap_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}")
        add_test(NAME depthxr_pivot_snap_tests COMMAND depthxr_pivot_snap_tests)
        set_tests_properties(depthxr_pivot_snap_tests PROPERTIES TIMEOUT 30)

        add_executable(depthxr_pivot_gaze_tests tests/pivot_gaze_tests.cpp src/openxr_layer.cpp src/layer_entry.cpp)
        target_include_directories(depthxr_pivot_gaze_tests PRIVATE include)
        target_link_libraries(depthxr_pivot_gaze_tests PRIVATE depthxr_core depthxr_osd depthxr_framegen
            OpenXR::openxr_loader d3d11 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_pivot_gaze_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}")
        add_test(NAME depthxr_pivot_gaze_tests COMMAND depthxr_pivot_gaze_tests)
        set_tests_properties(depthxr_pivot_gaze_tests PROPERTIES TIMEOUT 15)

        add_executable(depthxr_eye_gaze_tests tests/eye_gaze_tests.cpp src/openxr_layer.cpp)
        target_include_directories(depthxr_eye_gaze_tests PRIVATE include)
        target_link_libraries(depthxr_eye_gaze_tests PRIVATE depthxr_core depthxr_osd depthxr_framegen
            OpenXR::openxr_loader d3d11 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_eye_gaze_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}")
        add_test(NAME depthxr_eye_gaze_tests COMMAND depthxr_eye_gaze_tests)
        set_tests_properties(depthxr_eye_gaze_tests PROPERTIES TIMEOUT 15)
        # Frame Generation's Direct3D 11 bridge through the layer's own hooks
        # (xrCreateApiLayerInstance on) against a stand-in runtime whose
        # swapchain images are Direct3D 12 textures, on WARP
        # (`depthxr_d3d11_bridge_layer_tests hw` runs it on the GPU). Only the
        # test sees the Direct3D 12 OpenXR types: the layer stays Direct3D 11
        # only, as it ships.
        add_executable(depthxr_d3d11_bridge_layer_tests tests/d3d11_bridge_layer_tests.cpp src/openxr_layer.cpp
            src/layer_entry.cpp)
        set_source_files_properties(tests/d3d11_bridge_layer_tests.cpp PROPERTIES
            COMPILE_DEFINITIONS XR_USE_GRAPHICS_API_D3D12)
        target_include_directories(depthxr_d3d11_bridge_layer_tests PRIVATE include)
        target_link_libraries(depthxr_d3d11_bridge_layer_tests PRIVATE depthxr_core depthxr_osd depthxr_framegen
            OpenXR::openxr_loader d3d11 d3d12 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_d3d11_bridge_layer_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}")
        add_test(NAME depthxr_d3d11_bridge_layer_tests COMMAND depthxr_d3d11_bridge_layer_tests)
        add_test(NAME depthxr_d3d11_bridge_layer_hw_tests COMMAND depthxr_d3d11_bridge_layer_tests hw)
        set_tests_properties(depthxr_d3d11_bridge_layer_tests depthxr_d3d11_bridge_layer_hw_tests PROPERTIES
                             TIMEOUT 180 SKIP_RETURN_CODE 77)

        # Khronos validation for the Vulkan bridge's tests: the Vulkan SDK's
        # layer, or a VkLayer_khronos_validation.dll named here, copied into the
        # build tree with a manifest of its own (tests run unvalidated, with a
        # warning, when there is none).
        set(DEPTHXR_VULKAN_VALIDATION_LAYER "" CACHE FILEPATH
            "VkLayer_khronos_validation.dll for the Vulkan bridge tests (default: the Vulkan SDK's)")
        set(_vk_layer_source "${DEPTHXR_VULKAN_VALIDATION_LAYER}")
        if(NOT _vk_layer_source AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/VkLayer_khronos_validation.dll")
            set(_vk_layer_source "$ENV{VULKAN_SDK}/Bin/VkLayer_khronos_validation.dll")
        endif()
        set(_vk_layer_dir "")
        if(_vk_layer_source AND EXISTS "${_vk_layer_source}")
            set(_vk_layer_dir "${CMAKE_CURRENT_BINARY_DIR}/vulkan_layers")
            file(MAKE_DIRECTORY "${_vk_layer_dir}")
            file(COPY "${_vk_layer_source}" DESTINATION "${_vk_layer_dir}")
            get_filename_component(_vk_layer_name "${_vk_layer_source}" NAME)
            file(WRITE "${_vk_layer_dir}/VkLayer_khronos_validation.json"
"{
    \"file_format_version\": \"1.2.0\",
    \"layer\": {
        \"name\": \"VK_LAYER_KHRONOS_validation\",
        \"type\": \"GLOBAL\",
        \"library_path\": \".\\\\${_vk_layer_name}\",
        \"api_version\": \"1.3.290\",
        \"implementation_version\": \"1\",
        \"description\": \"Khronos validation layer for the VectorXR tests\",
        \"instance_extensions\": [
            { \"name\": \"VK_EXT_debug_report\", \"spec_version\": \"9\" },
            { \"name\": \"VK_EXT_debug_utils\", \"spec_version\": \"2\" },
            { \"name\": \"VK_EXT_layer_settings\", \"spec_version\": \"2\" },
            { \"name\": \"VK_EXT_validation_features\", \"spec_version\": \"6\" }
        ],
        \"device_extensions\": [
            { \"name\": \"VK_EXT_tooling_info\", \"spec_version\": \"1\",
              \"entrypoints\": [ \"vkGetPhysicalDeviceToolPropertiesEXT\" ] }
        ]
    }
}
")
            message(STATUS "Vulkan bridge tests validate with ${_vk_layer_source}")
        else()
            message(STATUS "Vulkan bridge tests: no Khronos validation layer (set DEPTHXR_VULKAN_VALIDATION_LAYER)")
        endif()
        file(TO_NATIVE_PATH "${_vk_layer_dir}" _vk_layer_dir_native)
        string(REPLACE "\\" "\\\\" _vk_layer_dir_define "${_vk_layer_dir_native}")

        # Frame Generation's Vulkan bridge: its pure parts, and its shared images
        # and fences on a GPU (there is no software Vulkan device that shares
        # memory with Direct3D 12, so there is no WARP variant).
        add_executable(depthxr_vulkan_bridge_tests tests/vulkan_bridge_tests.cpp)
        target_link_libraries(depthxr_vulkan_bridge_tests PRIVATE depthxr_framegen d3d12 dxgi)
        target_compile_definitions(depthxr_vulkan_bridge_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
            XR_USE_GRAPHICS_API_D3D12 XR_USE_GRAPHICS_API_VULKAN
            "DEPTHXR_VULKAN_LAYER_DIR=\"${_vk_layer_dir_define}\"")
        add_test(NAME depthxr_vulkan_bridge_tests COMMAND depthxr_vulkan_bridge_tests)
        set_tests_properties(depthxr_vulkan_bridge_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        # The Vulkan bridge through the layer's own hooks with a Vulkan game
        # against the stand-in runtime of the Direct3D 11 bridge's layer tests.
        # Only the test sees the Vulkan and Direct3D 12 OpenXR types.
        add_executable(depthxr_vulkan_bridge_layer_tests tests/vulkan_bridge_layer_tests.cpp src/openxr_layer.cpp
            src/layer_entry.cpp)
        set_source_files_properties(tests/vulkan_bridge_layer_tests.cpp PROPERTIES
            COMPILE_DEFINITIONS "XR_USE_GRAPHICS_API_D3D12;XR_USE_GRAPHICS_API_VULKAN")
        target_include_directories(depthxr_vulkan_bridge_layer_tests PRIVATE include)
        target_link_libraries(depthxr_vulkan_bridge_layer_tests PRIVATE depthxr_core depthxr_osd depthxr_framegen
            OpenXR::openxr_loader d3d11 d3d12 d3dcompiler dxgi shlwapi)
        target_compile_definitions(depthxr_vulkan_bridge_layer_tests PRIVATE DEPTHXR_TESTING
            WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D11 VECTORXR_VERSION="${_vectorxr_version}"
            "DEPTHXR_VULKAN_LAYER_DIR=\"${_vk_layer_dir_define}\"")
        add_test(NAME depthxr_vulkan_bridge_layer_tests COMMAND depthxr_vulkan_bridge_layer_tests)
        set_tests_properties(depthxr_vulkan_bridge_layer_tests PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
        if(DEPTHXR_REQUIRE_HW_TESTS)
            set_tests_properties(depthxr_vulkan_bridge_tests depthxr_vulkan_bridge_layer_tests PROPERTIES
                                 ENVIRONMENT "VECTORXR_TESTS_REQUIRE_HW=1")
        endif()
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
    add_executable(depthxr_turbo_trace_tests tests/turbo_trace_tests.cpp)
    target_link_libraries(depthxr_turbo_trace_tests PRIVATE depthxr_core)
    add_test(NAME depthxr_turbo_trace_tests COMMAND depthxr_turbo_trace_tests)
    set_tests_properties(depthxr_turbo_trace_tests PROPERTIES TIMEOUT 15)

    add_executable(depthxr_turbo_recovery_tests tests/turbo_recovery_tests.cpp)
    target_link_libraries(depthxr_turbo_recovery_tests PRIVATE depthxr_core)
    add_test(NAME depthxr_turbo_recovery_tests COMMAND depthxr_turbo_recovery_tests)
    set_tests_properties(depthxr_turbo_recovery_tests PROPERTIES TIMEOUT 10)
    add_executable(depthxr_path_unicode_tests tests/path_unicode_tests.cpp)
    target_link_libraries(depthxr_path_unicode_tests PRIVATE depthxr_core)
    add_test(NAME depthxr_path_unicode_tests COMMAND depthxr_path_unicode_tests)
    set_tests_properties(depthxr_path_unicode_tests PROPERTIES TIMEOUT 10)
    add_executable(depthxr_thread_census_tests tests/thread_census_tests.cpp)
    target_link_libraries(depthxr_thread_census_tests PRIVATE depthxr_core)
    add_test(NAME depthxr_thread_census_tests COMMAND depthxr_thread_census_tests)
    set_tests_properties(depthxr_thread_census_tests PROPERTIES TIMEOUT 10)

    add_executable(
        depthxr_action_set_attachment_tests
        tests/action_set_attachment_tests.cpp
    )
    target_include_directories(depthxr_action_set_attachment_tests PRIVATE include)
    add_test(NAME depthxr_action_set_attachment_tests COMMAND depthxr_action_set_attachment_tests)
endif()
