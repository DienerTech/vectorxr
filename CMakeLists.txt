cmake_minimum_required(VERSION 3.28)

project(VectorXR VERSION 0.5.0 LANGUAGES CXX)

option(DEPTHXR_BUILD_LAYER "Build the Windows OpenXR API layer" ON)
option(DEPTHXR_BUILD_TESTS "Build VectorXR layer tests" ON)
option(DEPTHXR_BUILD_HARDWARE_PROBES "Build opt-in real-runtime OpenXR diagnostic clients" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The layer carries its own C and C++ runtime (/MT). Linked as a DLL, it bound to whichever
# msvcp140.dll the game had loaded or ships beside its exe (one older than 14.38 crashes the
# game in xrCreateInstance on the first lock), could not load at all on a PC without the VC++
# redistributable (failing xrCreateInstance in every OpenXR app), and read and wrote numbers
# in the game's C locale. Every target must agree.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(CTest)

add_subdirectory(layer)
