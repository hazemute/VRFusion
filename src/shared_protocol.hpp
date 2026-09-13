#pragma once

#include <windows.h>
#include <cstdint>

namespace vrfusion {

inline constexpr wchar_t kSharedMapName[] = L"Local\\VRFusionSharedTexture_v2";
inline constexpr wchar_t kSharedEventName[] = L"Local\\VRFusionFrameReady_v2";
inline constexpr uint32_t kSharedMagic = 0x32524656u; // "VFR2" little-endian
inline constexpr uint32_t kSharedVersion = 2;

enum SharedFrameFlags : uint32_t {
    SharedFrame_ProducerAlive = 1u << 0,
    SharedFrame_DepthAware = 1u << 1,
    SharedFrame_OpenXR = 1u << 2,
    SharedFrame_SteamVR = 1u << 3,
};

struct alignas(8) SharedFrameInfo {
    uint32_t magic = kSharedMagic;
    uint32_t version = kSharedVersion;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgiFormat = 0;
    uint32_t producerPid = 0;
    uint32_t adapterLuidLow = 0;
    int32_t adapterLuidHigh = 0;
    uint64_t sharedHandle = 0;

    volatile LONG64 frameCounter = 0;
    volatile LONG64 sourceFrameCounter = 0;
    volatile LONG64 producerQpc = 0;
    volatile LONG64 renderDurationQpc = 0;
    volatile LONG64 droppedPublishFrames = 0;
    LONG64 qpcFrequency = 0;
    uint32_t flags = SharedFrame_ProducerAlive;
    uint32_t reserved = 0;
};
static_assert(sizeof(SharedFrameInfo) == 96);

} // namespace vrfusion
