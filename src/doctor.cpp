#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <iterator>

#include "openxr_capture_protocol.hpp"
#include "shared_protocol.hpp"
#include "control_protocol.hpp"

namespace {

bool ProcessRunning(const wchar_t* wanted) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{}; e.dwSize = sizeof(e);
    bool found = false;
    if (Process32FirstW(snap, &e)) {
        do { if (_wcsicmp(e.szExeFile, wanted) == 0) { found = true; break; } } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

bool LayerRegistered() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool found = false;
    for (DWORD index = 0;; ++index) {
        wchar_t name[2048]{}; DWORD nameChars = static_cast<DWORD>(std::size(name)); DWORD type = 0; DWORD data = 0; DWORD dataSize = sizeof(data);
        LONG r = RegEnumValueW(key, index, name, &nameChars, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        std::wstring n(name, nameChars);
        if ((n.find(L"VRFusion") != std::wstring::npos || n.find(L"VRFUSION") != std::wstring::npos) &&
            type == REG_DWORD && data == 0) { found = true; break; }
    }
    RegCloseKey(key);
    return found;
}


bool ControllerActive() {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::control::kControlMapName);
    if (!map) return false;
    auto* info = reinterpret_cast<const vrfusion::control::SharedControlInfo*>(
        MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(vrfusion::control::SharedControlInfo)));
    bool active = false;
    if (info && info->magic == vrfusion::control::kControlMagic && info->version == vrfusion::control::kControlVersion) {
        const ULONGLONG now = GetTickCount64();
        MemoryBarrier();
        const LONG64 heartbeat = info->heartbeatTickMs;
        active = (info->flags & vrfusion::control::Control_Active) != 0 && heartbeat > 0 &&
                 now >= static_cast<ULONGLONG>(heartbeat) &&
                 now - static_cast<ULONGLONG>(heartbeat) <= vrfusion::control::kHeartbeatTimeoutMs;
    }
    if (info) UnmapViewOfFile(info);
    CloseHandle(map);
    return active;
}

struct CaptureStatus {
    bool present = false;
    bool valid = false;
    bool advancing = false;
    bool color = false;
    bool depth = false;
    LONG64 first = 0;
    LONG64 second = 0;
};

CaptureStatus CheckOpenXR() {
    CaptureStatus s{};
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::xripc::kCaptureMapName);
    if (!map) return s;
    s.present = true;
    auto* info = reinterpret_cast<const vrfusion::xripc::SharedCaptureInfo*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(*info)));
    if (info) {
        s.valid = info->magic == vrfusion::xripc::kCaptureMagic && info->version == vrfusion::xripc::kCaptureVersion;
        if (s.valid) {
            s.first = info->frameCounter;
            s.color = (info->flags & (vrfusion::xripc::Capture_LeftColorValid | vrfusion::xripc::Capture_RightColorValid)) ==
                      (vrfusion::xripc::Capture_LeftColorValid | vrfusion::xripc::Capture_RightColorValid);
            s.depth = (info->flags & vrfusion::xripc::Capture_DepthAwareReady) != 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            s.second = info->frameCounter;
            s.advancing = s.second > s.first;
        }
        UnmapViewOfFile(info);
    }
    CloseHandle(map);
    return s;
}

void CheckOutput() {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::kSharedMapName);
    if (!map) { std::wcout << L"Final GPU output: NOT PRESENT\n"; return; }
    auto* info = reinterpret_cast<const vrfusion::SharedFrameInfo*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(*info)));
    if (!info || info->magic != vrfusion::kSharedMagic || info->version != vrfusion::kSharedVersion) {
        std::wcout << L"Final GPU output: PRESENT BUT INCOMPATIBLE\n";
    } else {
        const LONG64 frames = info->frameCounter;
        const LONG64 sourceFrames = info->sourceFrameCounter;
        const LONG64 drops = info->droppedPublishFrames;
        const wchar_t* backend = (info->flags & vrfusion::SharedFrame_OpenXR) ? L"OpenXR" :
                                 ((info->flags & vrfusion::SharedFrame_SteamVR) ? L"SteamVR" : L"Unknown");
        std::wcout << L"Final GPU output: READY, " << info->width << L"x" << info->height
                   << L", backend=" << backend
                   << L", depth=" << ((info->flags & vrfusion::SharedFrame_DepthAware) ? L"YES" : L"NO")
                   << L", source=" << sourceFrames << L", published=" << frames
                   << L", publish-drops=" << drops << L"\n";
        if (info->qpcFrequency > 0) {
            LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
            const LONG64 qpc = info->producerQpc;
            const double ageMs = qpc > 0 ? 1000.0 * double(now.QuadPart - qpc) / double(info->qpcFrequency) : -1.0;
            const LONG64 rq = info->renderDurationQpc;
            const double renderMs = 1000.0 * double(rq) / double(info->qpcFrequency);
            std::wcout << L"Output telemetry: age=" << ageMs << L" ms, CPU compositor=" << renderMs << L" ms\n";
        }
    }
    if (info) UnmapViewOfFile(info);
    CloseHandle(map);
}

} // namespace

int wmain() {
    std::wcout << L"VRFusion 0.5 Doctor\n====================\n";
    std::wcout << L"OpenXR layer registered: " << (LayerRegistered() ? L"YES" : L"NO") << L"\n";
    std::wcout << L"VRFusion controller heartbeat: " << (ControllerActive() ? L"ACTIVE" : L"NOT ACTIVE") << L"\n";
    std::wcout << L"SteamVR vrserver.exe: " << (ProcessRunning(L"vrserver.exe") ? L"RUNNING" : L"NOT RUNNING") << L"\n";

    const auto xr = CheckOpenXR();
    std::wcout << L"OpenXR capture map: " << (xr.present ? L"PRESENT" : L"NOT PRESENT") << L"\n";
    if (xr.present) {
        std::wcout << L"OpenXR metadata: " << (xr.valid ? L"VALID" : L"INVALID") << L"\n";
        if (xr.valid) {
            std::wcout << L"OpenXR stereo color: " << (xr.color ? L"READY" : L"WAITING") << L"\n";
            std::wcout << L"OpenXR depth: " << (xr.depth ? L"READY" : L"FALLBACK") << L"\n";
            std::wcout << L"OpenXR frames advancing: " << (xr.advancing ? L"YES" : L"NO") << L" (" << xr.first << L" -> " << xr.second << L")\n";
        }
    }

    CheckOutput();
    std::wcout << L"\nRecommendation: ";
    if (!ControllerActive()) std::wcout << L"run VRFusion.exe; it performs setup and backend selection automatically";
    else if (xr.valid && xr.color && xr.advancing) std::wcout << L"OpenXR capture is healthy; VRFusion.exe should use it automatically";
    else if (ProcessRunning(L"vrserver.exe")) std::wcout << L"SteamVR is available as the automatic fallback";
    else std::wcout << L"start a VR game; VRFusion.exe is already waiting for it";
    std::wcout << L"\n";
    return 0;
}
