cmake_minimum_required(VERSION 3.24)
project(VRFusion VERSION 0.5.0 LANGUAGES CXX)

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

add_executable(VRFusionSteamVR WIN32
    src/main.cpp
    src/shared_protocol.hpp
)

target_include_directories(VRFusionSteamVR PRIVATE
    "${openvr_sdk_SOURCE_DIR}/headers"
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(VRFusionSteamVR PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(VRFusionSteamVR PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

target_link_libraries(VRFusionSteamVR PRIVATE
    "${OPENVR_LIB}"
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
)


add_executable(VRFusion WIN32
    src/app.cpp
    src/stable_output.cpp
    src/stable_output.hpp
    src/control_protocol.hpp
    src/openxr_capture_protocol.hpp
    src/shared_protocol.hpp
)
target_include_directories(VRFusion PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(VRFusion PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(VRFusion PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(VRFusion PRIVATE user32 advapi32 shell32 d3d11 dxgi d3dcompiler)

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

add_custom_command(TARGET VRFusionSteamVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENVR_DLL}"
        "$<TARGET_FILE_DIR:VRFusionSteamVR>/openvr_api.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/vrfusion.ini"
        "$<TARGET_FILE_DIR:VRFusionSteamVR>/vrfusion.ini"
)


add_executable(VRFusionLauncher WIN32
    src/launcher.cpp
    src/openxr_capture_protocol.hpp
    src/control_protocol.hpp
)
target_include_directories(VRFusionLauncher PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(VRFusionLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(VRFusionLauncher PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(VRFusionLauncher PRIVATE user32)

add_executable(VRFusionDoctor
    src/doctor.cpp
    src/openxr_capture_protocol.hpp
    src/shared_protocol.hpp
    src/control_protocol.hpp
)
target_include_directories(VRFusionDoctor PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(VRFusionDoctor PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(VRFusionDoctor PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(VRFusionDoctor PRIVATE advapi32)

# OpenXR 1.1 headers for the depth-aware API layer backend.
FetchContent_Declare(
    openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(openxr_sdk)
if(NOT openxr_sdk_POPULATED)
    FetchContent_Populate(openxr_sdk)
endif()
set(OPENXR_INCLUDE_DIR "${openxr_sdk_SOURCE_DIR}/include")
if(NOT EXISTS "${OPENXR_INCLUDE_DIR}/openxr/openxr.h")
    message(FATAL_ERROR "OpenXR headers were not found: ${OPENXR_INCLUDE_DIR}")
endif()

add_library(VRFusionOpenXRLayer SHARED
    src/openxr_layer.cpp
    src/openxr_capture_protocol.hpp
    src/control_protocol.hpp
)
target_include_directories(VRFusionOpenXRLayer PRIVATE
    "${OPENXR_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)
target_compile_definitions(VRFusionOpenXRLayer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(VRFusionOpenXRLayer PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(VRFusionOpenXRLayer PRIVATE d3d11 dxgi)
set_target_properties(VRFusionOpenXRLayer PROPERTIES PREFIX "")

add_executable(VRFusionXRView WIN32
    src/xr_view.cpp
    src/openxr_capture_protocol.hpp
    src/shared_protocol.hpp
)
target_include_directories(VRFusionXRView PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(VRFusionXRView PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(VRFusionXRView PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
target_link_libraries(VRFusionXRView PRIVATE d3d11 dxgi d3dcompiler user32 gdi32)

add_custom_command(TARGET VRFusionOpenXRLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/openxr/XR_APILAYER_VRFusion_capture.json"
        "$<TARGET_FILE_DIR:VRFusionOpenXRLayer>/XR_APILAYER_VRFusion_capture.json"
)

add_executable(VRFusionOpenXRProbe
    src/openxr_probe.cpp
    src/openxr_capture_protocol.hpp
)
target_include_directories(VRFusionOpenXRProbe PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(VRFusionOpenXRProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(VRFusionOpenXRProbe PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
