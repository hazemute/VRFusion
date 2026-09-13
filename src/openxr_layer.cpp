#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "openxr_capture_protocol.hpp"
#include "control_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr const char* kLayerName = "XR_APILAYER_VRFusion_capture";
constexpr uint32_t kRoleColor = 1u << 0;
constexpr uint32_t kRoleDepth = 1u << 1;

struct InstanceDispatch {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
};

struct InstanceState {
    XrInstance instance = XR_NULL_HANDLE;
    InstanceDispatch next{};
};

struct SessionState {
    XrSession session = XR_NULL_HANDLE;
    std::shared_ptr<InstanceState> instance;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
    bool d3d11 = false;
};

struct SharedSlice {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    HANDLE sharedHandle = nullptr;
    bool hasFrame = false;
};

struct SwapchainState {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::shared_ptr<SessionState> session;
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::vector<ComPtr<ID3D11Texture2D>> images;
    std::deque<uint32_t> acquired;
    std::deque<uint32_t> waited;
    std::vector<SharedSlice> slices;
    uint32_t roles = 0;
    DXGI_FORMAT sharedFormat = DXGI_FORMAT_UNKNOWN;
    std::mutex mutex;
};

std::mutex gMutex;
std::unordered_map<XrInstance, std::shared_ptr<InstanceState>> gInstances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> gSessions;
std::unordered_map<XrSwapchain, std::shared_ptr<SwapchainState>> gSwapchains;

HANDLE gCaptureMapping = nullptr;
HANDLE gCaptureEvent = nullptr;
vrfusion::xripc::SharedCaptureInfo* gCaptureInfo = nullptr;
std::mutex gLogMutex;
std::mutex gIpcMutex;
std::mutex gControlMutex;

HANDLE gControlMapping = nullptr;
const vrfusion::control::SharedControlInfo* gControlInfo = nullptr;
ULONGLONG gNextControlProbeMs = 0;

void CloseControlIpc() {
    std::lock_guard<std::mutex> lock(gControlMutex);
    if (gControlInfo) {
        UnmapViewOfFile(gControlInfo);
        gControlInfo = nullptr;
    }
    if (gControlMapping) {
        CloseHandle(gControlMapping);
        gControlMapping = nullptr;
    }
}

bool CaptureRequested() {
    std::lock_guard<std::mutex> lock(gControlMutex);
    const ULONGLONG now = GetTickCount64();
    if (!gControlInfo && now >= gNextControlProbeMs) {
        gNextControlProbeMs = now + 500;
        gControlMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::control::kControlMapName);
        if (gControlMapping) {
            gControlInfo = reinterpret_cast<const vrfusion::control::SharedControlInfo*>(
                MapViewOfFile(gControlMapping, FILE_MAP_READ, 0, 0, sizeof(vrfusion::control::SharedControlInfo)));
            if (!gControlInfo) {
                CloseHandle(gControlMapping);
                gControlMapping = nullptr;
            }
        }
    }
    if (!gControlInfo) return false;
    if (gControlInfo->magic != vrfusion::control::kControlMagic ||
        gControlInfo->version != vrfusion::control::kControlVersion) return false;
    MemoryBarrier();
    const LONG64 heartbeat = gControlInfo->heartbeatTickMs;
    const uint32_t flags = gControlInfo->flags;
    if ((flags & vrfusion::control::Control_Active) == 0 || heartbeat <= 0) return false;
    const ULONGLONG heartbeatMs = static_cast<ULONGLONG>(heartbeat);
    return now >= heartbeatMs && (now - heartbeatMs) <= vrfusion::control::kHeartbeatTimeoutMs;
}

void Log(const std::string& text) {
    OutputDebugStringA(("[VRFusion OpenXR] " + text + "\n").c_str());
    std::lock_guard<std::mutex> lock(gLogMutex);
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempPath)) return;
    std::wstring path = tempPath;
    path += L"VRFusion-OpenXRLayer.log";
    std::ofstream out(std::filesystem::path(path), std::ios::app);
    if (out) out << text << '\n';
}

template <typename T>
bool LoadProc(PFN_xrGetInstanceProcAddr gipa, XrInstance instance, const char* name, T& fn) {
    PFN_xrVoidFunction raw = nullptr;
    const XrResult r = gipa(instance, name, &raw);
    if (XR_FAILED(r) || !raw) return false;
    fn = reinterpret_cast<T>(raw);
    return true;
}

const XrBaseInStructure* FindInChain(const void* next, XrStructureType type) {
    auto* node = reinterpret_cast<const XrBaseInStructure*>(next);
    while (node) {
        if (node->type == type) return node;
        node = node->next;
    }
    return nullptr;
}

DXGI_FORMAT TypelessDepthFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        default:
            return format;
    }
}

bool EnsureCaptureIpc(const LUID& luid) {
    if (gCaptureInfo) return true;

    gCaptureMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(vrfusion::xripc::SharedCaptureInfo)),
        vrfusion::xripc::kCaptureMapName);
    if (!gCaptureMapping) return false;

    gCaptureInfo = reinterpret_cast<vrfusion::xripc::SharedCaptureInfo*>(
        MapViewOfFile(gCaptureMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrfusion::xripc::SharedCaptureInfo)));
    if (!gCaptureInfo) {
        CloseHandle(gCaptureMapping);
        gCaptureMapping = nullptr;
        return false;
    }

    gCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, vrfusion::xripc::kCaptureEventName);
    if (!gCaptureEvent) {
        UnmapViewOfFile(gCaptureInfo);
        gCaptureInfo = nullptr;
        CloseHandle(gCaptureMapping);
        gCaptureMapping = nullptr;
        return false;
    }

    *gCaptureInfo = {};
    gCaptureInfo->magic = vrfusion::xripc::kCaptureMagic;
    gCaptureInfo->version = vrfusion::xripc::kCaptureVersion;
    gCaptureInfo->producerPid = GetCurrentProcessId();
    gCaptureInfo->adapterLuidLow = luid.LowPart;
    gCaptureInfo->adapterLuidHigh = luid.HighPart;
    gCaptureInfo->flags = vrfusion::xripc::Capture_D3D11Backend;
    InterlockedExchange64(&gCaptureInfo->sequence, 0);
    InterlockedExchange64(&gCaptureInfo->frameCounter, 0);
    Log("Created OpenXR capture IPC objects");
    return true;
}

void ShutdownCaptureIpc() {
    if (gCaptureInfo) {
        UnmapViewOfFile(gCaptureInfo);
        gCaptureInfo = nullptr;
    }
    if (gCaptureMapping) {
        CloseHandle(gCaptureMapping);
        gCaptureMapping = nullptr;
    }
    if (gCaptureEvent) {
        CloseHandle(gCaptureEvent);
        gCaptureEvent = nullptr;
    }
}

bool CreateSharedSlices(SwapchainState& state, ID3D11Texture2D* source) {
    if (!source || !state.session || !state.session->device) return false;
    if (!state.slices.empty()) return true;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    const bool depthRole = (state.roles & kRoleDepth) != 0;
    if (sourceDesc.SampleDesc.Count > 1 && depthRole) {
        Log("MSAA depth swapchain detected; keeping color capture and falling back from depth reconstruction");
        return false;
    }
    if (sourceDesc.SampleDesc.Count > 1) {
        UINT formatSupport = 0;
        if (FAILED(state.session->device->CheckFormatSupport(sourceDesc.Format, &formatSupport)) ||
            (formatSupport & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) == 0) {
            Log("MSAA color swapchain format cannot be resolved; skipping this swapchain");
            return false;
        }
    }

    state.sharedFormat = depthRole ? TypelessDepthFormat(sourceDesc.Format) : sourceDesc.Format;
    const uint32_t sliceCount = std::max(1u, sourceDesc.ArraySize);
    state.slices.resize(sliceCount);

    for (uint32_t slice = 0; slice < sliceCount; ++slice) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = sourceDesc.Width;
        desc.Height = sourceDesc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = state.sharedFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        auto& shared = state.slices[slice];
        HRESULT hr = state.session->device->CreateTexture2D(&desc, nullptr, &shared.texture);
        if (FAILED(hr)) {
            Log("CreateTexture2D failed for shared OpenXR slice");
            state.slices.clear();
            return false;
        }
        hr = shared.texture.As(&shared.mutex);
        if (FAILED(hr)) {
            Log("IDXGIKeyedMutex unavailable on shared OpenXR slice");
            state.slices.clear();
            return false;
        }
        ComPtr<IDXGIResource> resource;
        hr = shared.texture.As(&resource);
        if (FAILED(hr) || FAILED(resource->GetSharedHandle(&shared.sharedHandle)) || !shared.sharedHandle) {
            Log("GetSharedHandle failed for OpenXR capture slice");
            state.slices.clear();
            return false;
        }
    }
    return true;
}

void CaptureSwapchainImage(SwapchainState& state, uint32_t imageIndex) {
    if (!state.session || !state.session->context || state.roles == 0) return;
    if (imageIndex >= state.images.size() || !state.images[imageIndex]) return;

    ID3D11Texture2D* source = state.images[imageIndex].Get();
    if (!CreateSharedSlices(state, source)) return;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    bool copiedAny = false;
    std::vector<IDXGIKeyedMutex*> locked;
    locked.reserve(state.slices.size());

    const uint32_t sliceCount = std::min<uint32_t>(sourceDesc.ArraySize, static_cast<uint32_t>(state.slices.size()));
    for (uint32_t slice = 0; slice < sliceCount; ++slice) {
        auto& dst = state.slices[slice];
        if (!dst.mutex || !dst.texture) continue;
        if (dst.mutex->AcquireSync(0, 0) != S_OK) continue;

        const UINT srcSubresource = D3D11CalcSubresource(0, slice, sourceDesc.MipLevels);
        if (sourceDesc.SampleDesc.Count > 1)
            state.session->context->ResolveSubresource(dst.texture.Get(), 0, source, srcSubresource, sourceDesc.Format);
        else
            state.session->context->CopySubresourceRegion(dst.texture.Get(), 0, 0, 0, 0, source, srcSubresource, nullptr);
        locked.push_back(dst.mutex.Get());
        dst.hasFrame = true;
        copiedAny = true;
    }

    if (copiedAny) state.session->context->Flush();
    for (IDXGIKeyedMutex* mutex : locked) mutex->ReleaseSync(1);
}

std::shared_ptr<SwapchainState> GetSwapchain(XrSwapchain swapchain) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gSwapchains.find(swapchain);
    return it == gSwapchains.end() ? nullptr : it->second;
}

std::shared_ptr<SessionState> GetSession(XrSession session) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gSessions.find(session);
    return it == gSessions.end() ? nullptr : it->second;
}

std::shared_ptr<InstanceState> GetInstance(XrInstance instance) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gInstances.find(instance);
    return it == gInstances.end() ? nullptr : it->second;
}

vrfusion::xripc::Pose ConvertPose(const XrPosef& pose) {
    vrfusion::xripc::Pose out{};
    out.orientation[0] = pose.orientation.x;
    out.orientation[1] = pose.orientation.y;
    out.orientation[2] = pose.orientation.z;
    out.orientation[3] = pose.orientation.w;
    out.position[0] = pose.position.x;
    out.position[1] = pose.position.y;
    out.position[2] = pose.position.z;
    return out;
}

vrfusion::xripc::Fov ConvertFov(const XrFovf& fov) {
    return {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
}

vrfusion::xripc::SubImage ConvertRect(const XrRect2Di& rect) {
    vrfusion::xripc::SubImage out{};
    out.offsetX = rect.offset.x;
    out.offsetY = rect.offset.y;
    out.width = rect.extent.width > 0 ? static_cast<uint32_t>(rect.extent.width) : 0u;
    out.height = rect.extent.height > 0 ? static_cast<uint32_t>(rect.extent.height) : 0u;
    return out;
}

const XrCompositionLayerDepthInfoKHR* FindDepth(const XrCompositionLayerProjectionView& view) {
    const auto* base = FindInChain(view.next, XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR);
    return reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(base);
}

bool PublishProjectionLayer(const XrCompositionLayerProjection& layer, const std::shared_ptr<SessionState>& session) {
    if (!session || !session->d3d11 || layer.viewCount < 2 || !layer.views) return false;
    const auto& left = layer.views[0];
    const auto& right = layer.views[1];
    const auto leftColor = GetSwapchain(left.subImage.swapchain);
    const auto rightColor = GetSwapchain(right.subImage.swapchain);
    if (!leftColor || !rightColor) return false;

    const auto* leftDepthInfo = FindDepth(left);
    const auto* rightDepthInfo = FindDepth(right);
    auto leftDepth = leftDepthInfo ? GetSwapchain(leftDepthInfo->subImage.swapchain) : nullptr;
    auto rightDepth = rightDepthInfo ? GetSwapchain(rightDepthInfo->subImage.swapchain) : nullptr;

    struct SliceSnapshot {
        uint64_t handle = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid = false;
    };

    auto markAndSnapshot = [](const std::shared_ptr<SwapchainState>& sc, uint32_t sliceIndex, uint32_t role) {
        SliceSnapshot out{};
        if (!sc) return out;
        std::lock_guard<std::mutex> lock(sc->mutex);
        sc->roles |= role;
        out.format = sc->sharedFormat;
        out.width = sc->createInfo.width;
        out.height = sc->createInfo.height;
        if (sliceIndex < sc->slices.size()) {
            const auto& slice = sc->slices[sliceIndex];
            if (slice.hasFrame && slice.sharedHandle) {
                out.handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slice.sharedHandle));
                out.valid = true;
            }
        }
        return out;
    };

    const SliceSnapshot lc = markAndSnapshot(leftColor, left.subImage.imageArrayIndex, kRoleColor);
    const SliceSnapshot rc = markAndSnapshot(rightColor, right.subImage.imageArrayIndex, kRoleColor);
    const SliceSnapshot ld = leftDepthInfo ? markAndSnapshot(leftDepth, leftDepthInfo->subImage.imageArrayIndex, kRoleDepth) : SliceSnapshot{};
    const SliceSnapshot rd = rightDepthInfo ? markAndSnapshot(rightDepth, rightDepthInfo->subImage.imageArrayIndex, kRoleDepth) : SliceSnapshot{};

    uint32_t flags = vrfusion::xripc::Capture_D3D11Backend;
    if (lc.valid) flags |= vrfusion::xripc::Capture_LeftColorValid;
    if (rc.valid) flags |= vrfusion::xripc::Capture_RightColorValid;
    if (ld.valid) flags |= vrfusion::xripc::Capture_LeftDepthValid;
    if (rd.valid) flags |= vrfusion::xripc::Capture_RightDepthValid;
    if (lc.valid && rc.valid && ld.valid && rd.valid) flags |= vrfusion::xripc::Capture_DepthAwareReady;

    std::lock_guard<std::mutex> ipcLock(gIpcMutex);
    if (!EnsureCaptureIpc(session->adapterLuid)) return false;
    InterlockedIncrement64(&gCaptureInfo->sequence);
    MemoryBarrier();
    gCaptureInfo->producerPid = GetCurrentProcessId();
    gCaptureInfo->flags = flags;
    gCaptureInfo->adapterLuidLow = session->adapterLuid.LowPart;
    gCaptureInfo->adapterLuidHigh = session->adapterLuid.HighPart;
    gCaptureInfo->colorFormat = static_cast<uint32_t>(lc.format);
    gCaptureInfo->depthFormat = static_cast<uint32_t>(ld.format);
    gCaptureInfo->leftColorHandle = lc.handle;
    gCaptureInfo->rightColorHandle = rc.handle;
    gCaptureInfo->leftDepthHandle = ld.handle;
    gCaptureInfo->rightDepthHandle = rd.handle;
    gCaptureInfo->colorWidth = lc.width;
    gCaptureInfo->colorHeight = lc.height;
    gCaptureInfo->depthWidth = ld.width;
    gCaptureInfo->depthHeight = ld.height;
    gCaptureInfo->leftColorRect = ConvertRect(left.subImage.imageRect);
    gCaptureInfo->rightColorRect = ConvertRect(right.subImage.imageRect);
    gCaptureInfo->leftDepthRect = leftDepthInfo ? ConvertRect(leftDepthInfo->subImage.imageRect) : vrfusion::xripc::SubImage{};
    gCaptureInfo->rightDepthRect = rightDepthInfo ? ConvertRect(rightDepthInfo->subImage.imageRect) : vrfusion::xripc::SubImage{};
    gCaptureInfo->leftPose = ConvertPose(left.pose);
    gCaptureInfo->rightPose = ConvertPose(right.pose);
    gCaptureInfo->leftFov = ConvertFov(left.fov);
    gCaptureInfo->rightFov = ConvertFov(right.fov);
    gCaptureInfo->leftDepthRange = leftDepthInfo ? vrfusion::xripc::DepthRange{leftDepthInfo->minDepth, leftDepthInfo->maxDepth, leftDepthInfo->nearZ, leftDepthInfo->farZ} : vrfusion::xripc::DepthRange{};
    gCaptureInfo->rightDepthRange = rightDepthInfo ? vrfusion::xripc::DepthRange{rightDepthInfo->minDepth, rightDepthInfo->maxDepth, rightDepthInfo->nearZ, rightDepthInfo->farZ} : vrfusion::xripc::DepthRange{};
    MemoryBarrier();
    InterlockedIncrement64(&gCaptureInfo->sequence);
    InterlockedIncrement64(&gCaptureInfo->frameCounter);
    SetEvent(gCaptureEvent);
    return true;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrDestroySwapchain(XrSwapchain swapchain);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo);
XRAPI_ATTR XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo);

} // namespace

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;

#define VRFUSION_INTERCEPT(fn) \
    if (std::strcmp(name, #fn) == 0) { \
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_##fn); \
        return XR_SUCCESS; \
    }

    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    VRFUSION_INTERCEPT(xrDestroyInstance)
    VRFUSION_INTERCEPT(xrCreateSession)
    VRFUSION_INTERCEPT(xrDestroySession)
    VRFUSION_INTERCEPT(xrCreateSwapchain)
    VRFUSION_INTERCEPT(xrDestroySwapchain)
    VRFUSION_INTERCEPT(xrEnumerateSwapchainImages)
    VRFUSION_INTERCEPT(xrAcquireSwapchainImage)
    VRFUSION_INTERCEPT(xrWaitSwapchainImage)
    VRFUSION_INTERCEPT(xrReleaseSwapchainImage)
    VRFUSION_INTERCEPT(xrEndFrame)
#undef VRFUSION_INTERCEPT

    auto state = GetInstance(instance);
    if (!state || !state->next.GetInstanceProcAddr) return XR_ERROR_HANDLE_INVALID;
    return state->next.GetInstanceProcAddr(instance, name, function);
}

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrCreateApiLayerInstance(
    const XrInstanceCreateInfo* info,
    const XrApiLayerCreateInfo* apiLayerInfo,
    XrInstance* instance) {
    if (!info || !apiLayerInfo || !apiLayerInfo->nextInfo || !instance) return XR_ERROR_INITIALIZATION_FAILED;

    XrApiLayerNextInfo* nextInfo = apiLayerInfo->nextInfo;
    if (!nextInfo->nextGetInstanceProcAddr || !nextInfo->nextCreateApiLayerInstance) return XR_ERROR_INITIALIZATION_FAILED;

    XrApiLayerCreateInfo nextLayerInfo = *apiLayerInfo;
    nextLayerInfo.nextInfo = nextInfo->next;

    const XrResult result = nextInfo->nextCreateApiLayerInstance(info, &nextLayerInfo, instance);
    if (XR_FAILED(result)) return result;

    auto state = std::make_shared<InstanceState>();
    state->instance = *instance;
    state->next.GetInstanceProcAddr = nextInfo->nextGetInstanceProcAddr;

    bool ok = true;
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrDestroyInstance", state->next.DestroyInstance);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrCreateSession", state->next.CreateSession);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrDestroySession", state->next.DestroySession);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrCreateSwapchain", state->next.CreateSwapchain);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrDestroySwapchain", state->next.DestroySwapchain);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrEnumerateSwapchainImages", state->next.EnumerateSwapchainImages);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrAcquireSwapchainImage", state->next.AcquireSwapchainImage);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrWaitSwapchainImage", state->next.WaitSwapchainImage);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrReleaseSwapchainImage", state->next.ReleaseSwapchainImage);
    ok &= LoadProc(state->next.GetInstanceProcAddr, *instance, "xrEndFrame", state->next.EndFrame);

    if (!ok) {
        Log("Failed to populate one or more required OpenXR dispatch functions");
        if (state->next.DestroyInstance) {
            state->next.DestroyInstance(*instance);
        }
        *instance = XR_NULL_HANDLE;
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gInstances[*instance] = state;
    }
    Log("OpenXR instance attached to VRFusion capture layer");
    return XR_SUCCESS;
}

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    const char* layerName,
    XrNegotiateApiLayerRequest* apiLayerRequest) {
    if (!loaderInfo || !apiLayerRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (layerName && std::strcmp(layerName, kLayerName) != 0) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->maxInterfaceVersion < 1 || loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = std::min<XrVersion>(loaderInfo->maxApiVersion, XR_CURRENT_API_VERSION);
    apiLayerRequest->getInstanceProcAddr = xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = xrCreateApiLayerInstance;
    return XR_SUCCESS;
}

namespace {

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance) {
    auto state = GetInstance(instance);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = state->next.DestroyInstance(instance);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(gMutex);
        gInstances.erase(instance);
        if (gInstances.empty()) {
            std::lock_guard<std::mutex> ipcLock(gIpcMutex);
            ShutdownCaptureIpc();
            CloseControlIpc();
        }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateSession(
    XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    auto inst = GetInstance(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;

    const XrResult result = inst->next.CreateSession(instance, createInfo, session);
    if (XR_FAILED(result) || !session) return result;

    auto state = std::make_shared<SessionState>();
    state->session = *session;
    state->instance = inst;

    const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(
        FindInChain(createInfo ? createInfo->next : nullptr, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR));
    if (binding && binding->device) {
        state->device = binding->device;
        binding->device->GetImmediateContext(&state->context);
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(state->device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc))) {
            state->adapterLuid = desc.AdapterLuid;
            state->d3d11 = true;
            Log("Attached D3D11 OpenXR session");
        }
    } else {
        Log("OpenXR session is not D3D11; VRFusion 0.5 forwards it without capture");
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSessions[*session] = state;
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session) {
    auto state = GetSession(session);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = state->instance->next.DestroySession(session);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto it = gSwapchains.begin(); it != gSwapchains.end();) {
            if (it->second && it->second->session && it->second->session->session == session) it = gSwapchains.erase(it);
            else ++it;
        }
        gSessions.erase(session);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateSwapchain(
    XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
    auto sess = GetSession(session);
    if (!sess) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = sess->instance->next.CreateSwapchain(session, createInfo, swapchain);
    if (XR_FAILED(result) || !swapchain || !createInfo) return result;

    auto state = std::make_shared<SwapchainState>();
    state->handle = *swapchain;
    state->session = sess;
    state->createInfo = *createInfo;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSwapchains[*swapchain] = state;
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrDestroySwapchain(XrSwapchain swapchain) {
    auto state = GetSwapchain(swapchain);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = state->session->instance->next.DestroySwapchain(swapchain);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> lock(gMutex);
        gSwapchains.erase(swapchain);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrEnumerateSwapchainImages(
    XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    auto state = GetSwapchain(swapchain);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = state->session->instance->next.EnumerateSwapchainImages(swapchain, capacity, count, images);
    if (XR_FAILED(result) || !state->session->d3d11 || capacity == 0 || !images || !count) return result;
    const uint32_t n = std::min(capacity, *count);
    if (n == 0) return result;
    if (images[0].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return result;

    auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    std::lock_guard<std::mutex> stateLock(state->mutex);
    state->images.clear();
    state->images.reserve(n);
    for (uint32_t i = 0; i < n; ++i) { ComPtr<ID3D11Texture2D> image = d3dImages[i].texture; state->images.push_back(std::move(image)); }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrAcquireSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index) {
    auto state = GetSwapchain(swapchain);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = state->session->instance->next.AcquireSwapchainImage(swapchain, acquireInfo, index);
    if (XR_SUCCEEDED(result) && index && CaptureRequested()) {
        std::lock_guard<std::mutex> stateLock(state->mutex);
        state->acquired.push_back(*index);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrWaitSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    auto state = GetSwapchain(swapchain);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    const XrResult result = state->session->instance->next.WaitSwapchainImage(swapchain, waitInfo);
    if (XR_SUCCEEDED(result) && CaptureRequested()) {
        std::lock_guard<std::mutex> stateLock(state->mutex);
        if (!state->acquired.empty()) {
            state->waited.push_back(state->acquired.front());
            state->acquired.pop_front();
        }
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrReleaseSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    auto state = GetSwapchain(swapchain);
    if (!state) return XR_ERROR_HANDLE_INVALID;

    const bool capture = CaptureRequested();
    {
        std::lock_guard<std::mutex> stateLock(state->mutex);
        if (capture && !state->waited.empty() && state->roles != 0)
            CaptureSwapchainImage(*state, state->waited.front());
        if (!capture) {
            state->acquired.clear();
            state->waited.clear();
        }
    }

    const XrResult result = state->session->instance->next.ReleaseSwapchainImage(swapchain, releaseInfo);
    if (capture && XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> stateLock(state->mutex);
        if (!state->waited.empty()) state->waited.pop_front();
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    auto state = GetSession(session);
    if (!state) return XR_ERROR_HANDLE_INVALID;

    if (CaptureRequested() && frameEndInfo && state->d3d11 && frameEndInfo->layers) {
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* base = frameEndInfo->layers[i];
            if (base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
                if (PublishProjectionLayer(*projection, state)) break;
            }
        }
    }

    return state->instance->next.EndFrame(session, frameEndInfo);
}

} // namespace
