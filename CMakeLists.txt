cmake_minimum_required(VERSION 3.24)
project(VRFusion VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VRFusion currently targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    openvr_sdk
    URL https://github.com/ValveSoftware/openvr/archive/refs/tags/v2.15.6.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(openvr_sdk)
if(NOT openvr_sdk_POPULATED)
    FetchContent_Populate(openvr_sdk)
endif()

set(OPENVR_LIB "${openvr_sdk_SOURCE_DIR}/lib/win64/openvr_api.lib")
set(OPENVR_DLL "${openvr_sdk_SOURCE_DIR}/bin/win64/openvr_api.dll")

if(NOT EXISTS "${OPENVR_LIB}")
    message(FATAL_ERROR "OpenVR import library was not found: ${OPENVR_LIB}")
endif()
if(NOT EXISTS "${OPENVR_DLL}")
    message(FATAL_ERROR "OpenVR runtime DLL was not found: ${OPENVR_DLL}")
endif()

add_executable(VRFusion WIN32
    src/main.cpp
    src/shared_protocol.hpp
)

target_include_directories(VRFusion PRIVATE
    "${openvr_sdk_SOURCE_DIR}/headers"
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(VRFusion PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(VRFusion PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

target_link_libraries(VRFusion PRIVATE
    "${OPENVR_LIB}"
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
)

add_executable(VRFusionSharedProbe
    src/shared_probe.cpp
    src/shared_protocol.hpp
)

target_include_directories(VRFusionSharedProbe PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(VRFusionSharedProbe PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(VRFusionSharedProbe PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

target_link_libraries(VRFusionSharedProbe PRIVATE
    d3d11
    dxgi
)

add_custom_command(TARGET VRFusion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:VRFusion>/openvr_api.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/vrfusion.ini"
        "$<TARGET_FILE_DIR:VRFusion>/vrfusion.ini"
)
