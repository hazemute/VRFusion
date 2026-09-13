#include <windows.h>
#include <iostream>
#include <iomanip>
#include "openxr_capture_protocol.hpp"

int wmain() {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::xripc::kCaptureMapName);
    if (!map) {
        std::wcerr << L"VRFusion OpenXR capture mapping not found. Start an enabled D3D11 OpenXR game first.\n";
        return 2;
    }
    auto* info = reinterpret_cast<const vrfusion::xripc::SharedCaptureInfo*>(
        MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(vrfusion::xripc::SharedCaptureInfo)));
    if (!info) { CloseHandle(map); return 3; }
    if (info->magic != vrfusion::xripc::kCaptureMagic || info->version != vrfusion::xripc::kCaptureVersion) {
        std::wcerr << L"Capture protocol mismatch.\n";
        UnmapViewOfFile(info); CloseHandle(map); return 4;
    }
    const LONG64 start = info->frameCounter;
    std::wcout << L"Producer PID: " << info->producerPid << L"\n"
               << L"Flags: 0x" << std::hex << info->flags << std::dec << L"\n"
               << L"Color handles: 0x" << std::hex << info->leftColorHandle << L" / 0x" << info->rightColorHandle << std::dec << L"\n"
               << L"Depth handles: 0x" << std::hex << info->leftDepthHandle << L" / 0x" << info->rightDepthHandle << std::dec << L"\n"
               << L"Color size: " << info->colorWidth << L"x" << info->colorHeight << L"\n"
               << L"Depth size: " << info->depthWidth << L"x" << info->depthHeight << L"\n"
               << L"Frame counter: " << start << L"\n";
    HANDLE evt = OpenEventW(SYNCHRONIZE, FALSE, vrfusion::xripc::kCaptureEventName);
    if (evt) WaitForSingleObject(evt, 2000); else Sleep(1000);
    const LONG64 end = info->frameCounter;
    const bool colors = (info->flags & (vrfusion::xripc::Capture_LeftColorValid | vrfusion::xripc::Capture_RightColorValid)) ==
                        (vrfusion::xripc::Capture_LeftColorValid | vrfusion::xripc::Capture_RightColorValid);
    std::wcout << L"Frame counter after wait: " << end << L"\n"
               << L"Color capture: " << (colors ? L"READY" : L"NOT READY") << L"\n"
               << L"Depth center view: " << ((info->flags & vrfusion::xripc::Capture_DepthAwareReady) ? L"READY" : L"FALLBACK") << L"\n";
    if (evt) CloseHandle(evt);
    UnmapViewOfFile(info); CloseHandle(map);
    if (!colors) return 5;
    if (end <= start) {
        std::wcerr << L"No advancing OpenXR frames observed.\n";
        return 6;
    }
    std::wcout << L"VRFusion OpenXR probe: PASS\n";
    return 0;
}
