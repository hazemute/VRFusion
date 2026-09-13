cmake_minimum_required(VERSION 3.24)
project(vrfusion_obs LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The VRFusion OBS source currently targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OBS_INCLUDE_DIR "" CACHE PATH "Directory containing obs-module.h (normally OBS source/libobs)")
set(OBS_LIB "" CACHE FILEPATH "Path to the matching obs.lib")

if(NOT EXISTS "${OBS_INCLUDE_DIR}/obs-module.h")
    message(FATAL_ERROR "Set OBS_INCLUDE_DIR to the OBS libobs include directory containing obs-module.h")
endif()
if(NOT EXISTS "${OBS_LIB}")
    message(FATAL_ERROR "Set OBS_LIB to the matching OBS import library (obs.lib)")
endif()

add_library(vrfusion-obs MODULE
    vrfusion-obs.cpp
    ../src/shared_protocol.hpp
)

target_include_directories(vrfusion-obs PRIVATE
    "${OBS_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

target_compile_definitions(vrfusion-obs PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(vrfusion-obs PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

target_link_libraries(vrfusion-obs PRIVATE "${OBS_LIB}")
set_target_properties(vrfusion-obs PROPERTIES PREFIX "" OUTPUT_NAME "vrfusion-obs")
