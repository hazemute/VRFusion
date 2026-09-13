#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>

#include "shared_protocol.hpp"

using Microsoft::WRL::ComPtr;

namespace {

bool SameLuid(const LUID& a, uint32_t low, int32_t high) {
    return a.LowPart == low && a.HighPart == high;
}

} // namespace

int wmain() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::kSharedMapName);
    if (!mapping) {
        std::wcerr << L"VRFusion shared mapping is not available. Start VRFusion first.\n";
        return 2;
    }

    auto* info = reinterpret_cast<const vrfusion::SharedFrameInfo*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(vrfusion::SharedFrameInfo)));
    if (!info) {
        std::wcerr << L"MapViewOfFile failed: " << GetLastError() << L"\n";
        CloseHandle(mapping);
        return 3;
    }

    if (info->magic != vrfusion::kSharedMagic || info->version != vrfusion::kSharedVersion) {
        std::wcerr << L"Unexpected VRFusion shared protocol version.\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 4;
    }

    std::wcout << L"Producer PID: " << info->producerPid << L"\n"
               << L"Output: " << info->width << L"x" << info->height << L"\n"
               << L"DXGI format: " << info->dxgiFormat << L"\n"
               << L"Frame counter: " << info->frameCounter << L"\n";

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::wcerr << L"CreateDXGIFactory1 failed.\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 5;
    }

    ComPtr<IDXGIAdapter1> matchingAdapter;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (SameLuid(desc.AdapterLuid, info->adapterLuidLow, info->adapterLuidHigh)) {
            matchingAdapter = adapter;
            std::wcout << L"Adapter: " << desc.Description << L"\n";
            break;
        }
    }

    if (!matchingAdapter) {
        std::wcerr << L"Could not find the producer GPU by LUID.\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 6;
    }

    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL created{};
    hr = D3D11CreateDevice(
        matchingAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &created, &context);
    if (FAILED(hr)) {
        std::wcerr << L"D3D11CreateDevice failed on producer GPU.\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 7;
    }

    const HANDLE sharedHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(info->sharedHandle));
    ComPtr<ID3D11Texture2D> texture;
    hr = device->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&texture));
    if (FAILED(hr) || !texture) {
        std::wcerr << L"OpenSharedResource failed. HRESULT=0x" << std::hex << static_cast<uint32_t>(hr) << L"\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 8;
    }

    ComPtr<IDXGIKeyedMutex> keyedMutex;
    hr = texture.As(&keyedMutex);
    if (FAILED(hr) || !keyedMutex) {
        std::wcerr << L"Shared resource does not expose IDXGIKeyedMutex.\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 9;
    }

    hr = keyedMutex->AcquireSync(1, 2000);
    if (hr != S_OK) {
        std::wcerr << L"Timed out waiting for producer frame key 1. HRESULT=0x"
                   << std::hex << static_cast<uint32_t>(hr) << L"\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 10;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const LONG64 before = info->frameCounter;
    std::wcout << L"Shared texture opened successfully: " << desc.Width << L"x" << desc.Height << L"\n"
               << L"Acquired synchronized frame: " << before << L"\n";

    HANDLE frameEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, vrfusion::kSharedEventName);
    if (frameEvent) ResetEvent(frameEvent);
    keyedMutex->ReleaseSync(0);

    bool advanced = false;
    if (frameEvent) {
        const DWORD wait = WaitForSingleObject(frameEvent, 2000);
        if (wait == WAIT_OBJECT_0 && keyedMutex->AcquireSync(1, 2000) == S_OK) {
            const LONG64 after = info->frameCounter;
            advanced = after > before;
            std::wcout << L"Frame event verified: " << before << L" -> " << after << L"\n";
            keyedMutex->ReleaseSync(0);
        }
        CloseHandle(frameEvent);
    } else {
        Sleep(50);
        if (keyedMutex->AcquireSync(1, 2000) == S_OK) {
            const LONG64 after = info->frameCounter;
            advanced = after > before;
            std::wcout << L"Frame counter after synchronization: " << before << L" -> " << after << L"\n";
            keyedMutex->ReleaseSync(0);
        }
    }

    if (!advanced) {
        std::wcerr << L"Shared texture opened, but producer did not advance a synchronized frame.\n";
        UnmapViewOfFile(info);
        CloseHandle(mapping);
        return 11;
    }

    std::wcout << L"VRFusion shared-output probe: PASS\n";
    UnmapViewOfFile(info);
    CloseHandle(mapping);
    return 0;
}
