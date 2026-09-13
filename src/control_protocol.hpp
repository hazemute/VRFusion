#pragma once

#include <windows.h>
#include <cstdint>

namespace vrfusion::control {

inline constexpr wchar_t kControlMapName[] = L"Local\\VRFusionControl_v1";
inline constexpr uint32_t kControlMagic = 0x31434656u; // "VFC1"
inline constexpr uint32_t kControlVersion = 1;
inline constexpr ULONGLONG kHeartbeatTimeoutMs = 2500;

enum ControlFlags : uint32_t {
    Control_Active = 1u << 0,
    Control_AutoBackend = 1u << 1,
};

struct alignas(8) SharedControlInfo {
    uint32_t magic = kControlMagic;
    uint32_t version = kControlVersion;
    uint32_t controllerPid = 0;
    uint32_t flags = Control_Active | Control_AutoBackend;
    volatile LONG64 heartbeatTickMs = 0;
    volatile LONG64 generation = 0;
    uint32_t reserved[4]{};
};
static_assert(sizeof(SharedControlInfo) == 48);

} // namespace vrfusion::control
