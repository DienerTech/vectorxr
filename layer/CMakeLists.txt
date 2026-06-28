add_library(
    depthxr_core
    STATIC
    src/config_parser.cpp
    src/input_devices.cpp
    src/seen_apps.cpp
    src/settings.cpp
    src/sound_player.cpp
    src/settings_resolver.cpp
    src/util/config_path.cpp
    src/util/logger.cpp
    src/util/process_info.cpp
    src/math/stereo_boost.cpp
    src/math/convergence.cpp
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
                $<TARGET_FILE_DIR:XR_APILAYER_DIENERTECH_VECTORXR>/sounds/
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
