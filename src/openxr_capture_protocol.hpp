#pragma once

#include <windows.h>
#include <cstdint>

namespace vrfusion::xripc {

inline constexpr wchar_t kCaptureMapName[] = L"Local\\VRFusionOpenXRCapture_v1";
inline constexpr wchar_t kCaptureEventName[] = L"Local\\VRFusionOpenXRFrame_v1";
inline constexpr uint32_t kCaptureMagic = 0x52584656u; // "VFXR"
inline constexpr uint32_t kCaptureVersion = 1;

enum CaptureFlags : uint32_t {
    Capture_LeftColorValid  = 1u << 0,
    Capture_RightColorValid = 1u << 1,
    Capture_LeftDepthValid  = 1u << 2,
    Capture_RightDepthValid = 1u << 3,
    Capture_DepthAwareReady = 1u << 4,
    Capture_D3D11Backend    = 1u << 5,
};

struct Pose {
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f}; // x,y,z,w
    float position[3]{0.0f, 0.0f, 0.0f};
    float pad = 0.0f;
};

struct Fov {
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

struct SubImage {
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DepthRange {
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
    float nearZ = 0.0f;
    float farZ = 0.0f;
};

struct SharedCaptureInfo {
    uint32_t magic = kCaptureMagic;
    uint32_t version = kCaptureVersion;
    uint32_t producerPid = 0;
    uint32_t flags = 0;

    uint32_t adapterLuidLow = 0;
    int32_t adapterLuidHigh = 0;
    uint32_t colorFormat = 0;
    uint32_t depthFormat = 0;

    uint64_t leftColorHandle = 0;
    uint64_t rightColorHandle = 0;
    uint64_t leftDepthHandle = 0;
    uint64_t rightDepthHandle = 0;

    uint32_t colorWidth = 0;
    uint32_t colorHeight = 0;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;

    SubImage leftColorRect{};
    SubImage rightColorRect{};
    SubImage leftDepthRect{};
    SubImage rightDepthRect{};

    Pose leftPose{};
    Pose rightPose{};
    Fov leftFov{};
    Fov rightFov{};
    DepthRange leftDepthRange{};
    DepthRange rightDepthRange{};

    volatile LONG64 sequence = 0;
    volatile LONG64 frameCounter = 0;
};

} // namespace vrfusion::xripc
