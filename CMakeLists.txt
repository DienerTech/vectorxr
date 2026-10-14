cmake_minimum_required(VERSION 3.28)

project(VectorXR VERSION 0.5.0 LANGUAGES CXX)

option(DEPTHXR_BUILD_LAYER "Build the Windows OpenXR API layer" ON)
option(DEPTHXR_BUILD_TESTS "Build VectorXR layer tests" ON)
option(DEPTHXR_BUILD_HARDWARE_PROBES "Build opt-in real-runtime OpenXR diagnostic clients" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The layer binds to whichever msvcp140.dll the game has loaded or ships beside its exe. Since
# VS 2022 17.10, std::mutex and condition_variable are built inline (constexpr), and a
# msvcp140 older than 14.38 then locks them through a null pointer: the game crashes in
# xrCreateInstance. Initialize them in the runtime instead. Every target must agree.
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

include(CTest)

add_subdirectory(layer)
