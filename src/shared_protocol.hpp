#pragma once

#include <windows.h>
#include <cstdint>

namespace vrfusion {

inline constexpr wchar_t kSharedMapName[] = L"Local\\VRFusionSharedTexture_v1";
inline constexpr wchar_t kSharedEventName[] = L"Local\\VRFusionFrameReady_v1";
inline constexpr uint32_t kSharedMagic = 0x31524656u; // "VFR1" little-endian
inline constexpr uint32_t kSharedVersion = 1;

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
};
static_assert(sizeof(SharedFrameInfo) == 48);

} // namespace vrfusion
