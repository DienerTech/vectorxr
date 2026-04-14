cmake_minimum_required(VERSION 3.28)

project(VectorXR VERSION 0.4.0 LANGUAGES CXX)

option(DEPTHXR_BUILD_LAYER "Build the Windows OpenXR API layer" ON)
option(DEPTHXR_BUILD_TESTS "Build VectorXR layer tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(layer)
