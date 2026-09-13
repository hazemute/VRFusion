#include "stable_output.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace vrfusion {
namespace {

constexpr wchar_t kOutputClass[] = L"VRFusionStableOutputWindow";
constexpr wchar_t kOutputTitle[] = L"VRFusion Output";

bool SameLuid(const LUID& luid, uint32_t low, int32_t high) {
    return luid.LowPart == low && luid.HighPart == high;
}

} // namespace

StableOutput::~StableOutput() {
    CloseMapping();
    ResetGpu();
    if (hwnd_) DestroyWindow(hwnd_);
}

bool StableOutput::Initialize(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &StableOutput::WndProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kOutputClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT rect{0, 0, 960, 540};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
    AdjustWindowRect(&rect, style, FALSE);
    hwnd_ = CreateWindowExW(0, kOutputClass, kOutputTitle, style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rect.right - rect.left, rect.bottom - rect.top,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_HIDE);
    return true;
}

void StableOutput::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void StableOutput::CloseMapping() {
    if (info_) {
        UnmapViewOfFile(info_);
        info_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool StableOutput::EnsureMapping() {
    if (info_ && info_->magic == kSharedMagic && info_->version == kSharedVersion) return true;
    CloseMapping();
    mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedMapName);
    if (!mapping_) return false;
    info_ = reinterpret_cast<const SharedFrameInfo*>(
        MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(SharedFrameInfo)));
    if (!info_ || info_->magic != kSharedMagic || info_->version != kSharedVersion) {
        CloseMapping();
        return false;
    }
    return true;
}

void StableOutput::ResetProducerTexture() {
    localSrv_.Reset();
    localTexture_.Reset();
    sharedMutex_.Reset();
    sharedTexture_.Reset();
    openedHandle_ = 0;
    lastFrame_ = -1;
}

void StableOutput::ResetGpu() {
    ResetProducerTexture();
    rtv_.Reset();
    swapchain_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    context_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    adapterLow_ = 0;
    adapterHigh_ = 0;
}

bool StableOutput::CreateGpuObjects(uint32_t adapterLow, int32_t adapterHigh) {
    ResetGpu();
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)))) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory_->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(candidate->GetDesc1(&desc)) && SameLuid(desc.AdapterLuid, adapterLow, adapterHigh)) {
            adapter_ = candidate;
            break;
        }
    }
    if (!adapter_) return false;

    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created{};
    if (FAILED(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                 &device_, &created, &context_))) return false;
    if (!CreateSwapchain() || !CreateShaders()) return false;
    adapterLow_ = adapterLow;
    adapterHigh_ = adapterHigh;
    return true;
}

bool StableOutput::CreateSwapchain() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = std::max<LONG>(1, client.right - client.left);
    desc.BufferDesc.Height = std::max<LONG>(1, client.bottom - client.top);
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd_;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory_->CreateSwapChain(device_.Get(), &desc, &swapchain_))) return false;
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    return CreateBackbuffer();
}

bool StableOutput::CreateBackbuffer() {
    if (!swapchain_) return false;
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_));
}

bool StableOutput::CreateShaders() {
    static constexpr char shader[] = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
Texture2D sourceTex : register(t0);
SamplerState linearSampler : register(s0);
float4 PSMain(VSOut i) : SV_TARGET { return sourceTex.Sample(linearSampler, i.uv); }
)";
    ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    if (FAILED(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
                          "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors))) return false;
    errors.Reset();
    if (FAILED(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
                          "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors))) return false;
    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_))) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device_->CreateSamplerState(&sd, &sampler_));
}

void StableOutput::ResizeIfNeeded() {
    if (!resizePending_ || !swapchain_) return;
    resizePending_ = false;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    rtv_.Reset();
    if (SUCCEEDED(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) CreateBackbuffer();
}

bool StableOutput::EnsureProducerTexture() {
    if (!EnsureMapping()) return false;
    const uint64_t handle = info_->sharedHandle;
    if (!handle) return false;
    if (!device_ || adapterLow_ != info_->adapterLuidLow || adapterHigh_ != info_->adapterLuidHigh) {
        if (!CreateGpuObjects(info_->adapterLuidLow, info_->adapterLuidHigh)) return false;
    }
    if (sharedTexture_ && openedHandle_ == handle) return true;

    ResetProducerTexture();
    HANDLE nativeHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    if (FAILED(device_->OpenSharedResource(nativeHandle, IID_PPV_ARGS(&sharedTexture_))) || !sharedTexture_) return false;
    if (FAILED(sharedTexture_.As(&sharedMutex_)) || !sharedMutex_) {
        ResetProducerTexture();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    sharedTexture_->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &localTexture_)) ||
        FAILED(device_->CreateShaderResourceView(localTexture_.Get(), nullptr, &localSrv_))) {
        ResetProducerTexture();
        return false;
    }
    openedHandle_ = handle;
    return true;
}

void StableOutput::DrawLocalTexture() {
    if (!device_ || !context_ || !swapchain_ || !rtv_) return;
    ResizeIfNeeded();
    if (!rtv_) return;

    RECT client{};
    GetClientRect(hwnd_, &client);
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(std::max<LONG>(1, client.right - client.left));
    vp.Height = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    constexpr float clear[4] = {0.015f, 0.018f, 0.022f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clear);
    if (localSrv_) {
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, localSrv_.GetAddressOf());
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        context_->PSSetShaderResources(0, 1, &nullSrv);
    }
    swapchain_->Present(0, 0);
}

void StableOutput::Render() {
    if (!hwnd_ || IsIconic(hwnd_)) return;
    if (!EnsureProducerTexture()) {
        if (device_ && swapchain_) DrawLocalTexture();
        return;
    }
    const LONG64 frame = info_ ? info_->frameCounter : -1;
    if (sharedMutex_ && frame != lastFrame_ && sharedMutex_->AcquireSync(1, 0) == S_OK) {
        context_->CopyResource(localTexture_.Get(), sharedTexture_.Get());
        sharedMutex_->ReleaseSync(0);
        lastFrame_ = frame;
    }
    DrawLocalTexture();
}

LRESULT CALLBACK StableOutput::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    StableOutput* self = reinterpret_cast<StableOutput*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<StableOutput*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    }
    return self ? self->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT StableOutput::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) resizePending_ = true;
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vrfusion
