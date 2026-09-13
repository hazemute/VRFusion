#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>

#include "shared_protocol.hpp"

namespace vrfusion {

class StableOutput {
public:
    StableOutput() = default;
    ~StableOutput();

    bool Initialize(HINSTANCE instance);
    void Render();
    void Show();
    HWND window() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool EnsureMapping();
    bool EnsureProducerTexture();
    bool CreateGpuObjects(uint32_t adapterLow, int32_t adapterHigh);
    bool CreateSwapchain();
    bool CreateShaders();
    bool CreateBackbuffer();
    void ResizeIfNeeded();
    void ResetProducerTexture();
    void ResetGpu();
    void CloseMapping();
    void DrawLocalTexture();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool resizePending_ = false;

    HANDLE mapping_ = nullptr;
    const SharedFrameInfo* info_ = nullptr;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> sharedMutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> localTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> localSrv_;

    uint64_t openedHandle_ = 0;
    uint32_t adapterLow_ = 0;
    int32_t adapterHigh_ = 0;
    LONG64 lastFrame_ = -1;
};

} // namespace vrfusion
