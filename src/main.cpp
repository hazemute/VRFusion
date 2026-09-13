#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <openvr.h>

#include "shared_protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"VRFusionWindowClass";
constexpr wchar_t kWindowTitle[] = L"VRFusion Spectator";
constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Mat3 {
    float m[3][3]{};
};

struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Mat3 Identity3() {
    Mat3 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
    return r;
}

Mat3 RotationOf(const vr::HmdMatrix34_t& m) {
    Mat3 r{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) r.m[row][col] = m.m[row][col];
    }
    return r;
}

Mat3 Transpose(const Mat3& a) {
    Mat3 r{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) r.m[row][col] = a.m[col][row];
    }
    return r;
}

Mat3 Multiply(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            r.m[row][col] = a.m[row][0] * b.m[0][col] +
                            a.m[row][1] * b.m[1][col] +
                            a.m[row][2] * b.m[2][col];
        }
    }
    return r;
}

Vec3 Transform(const Mat3& a, const Vec3& v) {
    return {
        a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
        a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
        a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z,
    };
}

Quat Normalize(Quat q) {
    const float length = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (length <= 1e-8f) return {};
    const float inv = 1.0f / length;
    q.w *= inv; q.x *= inv; q.y *= inv; q.z *= inv;
    return q;
}

float Dot(const Quat& a, const Quat& b) {
    return a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
}

Quat QuatFromMat3(const Mat3& m) {
    Quat q{};
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return Normalize(q);
}

Mat3 Mat3FromQuat(const Quat& in) {
    const Quat q = Normalize(in);
    const float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    const float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    const float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    Mat3 r{};
    r.m[0][0] = 1.0f - 2.0f*(yy + zz);
    r.m[0][1] = 2.0f*(xy - wz);
    r.m[0][2] = 2.0f*(xz + wy);
    r.m[1][0] = 2.0f*(xy + wz);
    r.m[1][1] = 1.0f - 2.0f*(xx + zz);
    r.m[1][2] = 2.0f*(yz - wx);
    r.m[2][0] = 2.0f*(xz - wy);
    r.m[2][1] = 2.0f*(yz + wx);
    r.m[2][2] = 1.0f - 2.0f*(xx + yy);
    return r;
}

Quat Slerp(Quat a, Quat b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float cosTheta = Dot(a, b);
    if (cosTheta < 0.0f) {
        b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z;
        cosTheta = -cosTheta;
    }
    if (cosTheta > 0.9995f) {
        Quat r{
            a.w + t*(b.w-a.w), a.x + t*(b.x-a.x),
            a.y + t*(b.y-a.y), a.z + t*(b.z-a.z)
        };
        return Normalize(r);
    }
    const float theta = std::acos(std::clamp(cosTheta, -1.0f, 1.0f));
    const float sinTheta = std::sin(theta);
    const float wa = std::sin((1.0f-t)*theta) / sinTheta;
    const float wb = std::sin(t*theta) / sinTheta;
    return Normalize({
        a.w*wa + b.w*wb,
        a.x*wa + b.x*wb,
        a.y*wa + b.y*wb,
        a.z*wa + b.z*wb
    });
}

float AngularDistance(const Quat& a, const Quat& b) {
    const float d = std::clamp(std::fabs(Dot(Normalize(a), Normalize(b))), 0.0f, 1.0f);
    return 2.0f * std::acos(d);
}

enum class ViewMode : uint32_t {
    Fusion = 0,
    Left = 1,
    Right = 2,
    SideBySide = 3,
};

const wchar_t* ViewModeName(ViewMode mode) {
    switch (mode) {
        case ViewMode::Fusion: return L"Fusion";
        case ViewMode::Left: return L"Left eye";
        case ViewMode::Right: return L"Right eye";
        case ViewMode::SideBySide: return L"Side-by-side";
        default: return L"Unknown";
    }
}

struct Config {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    float feather = 0.020f;
    float seamContrast = 4.0f;
    float zoom = 1.08f;
    bool stabilization = true;
    float stabilizationMs = 75.0f;
    float maxStabilizationDeg = 7.0f;
    bool showFps = true;
    bool vsync = false;
    ViewMode mode = ViewMode::Fusion;
};

struct FovTangents {
    float left = -1.0f;
    float right = 1.0f;
    float top = -1.0f;
    float bottom = 1.0f;
};

struct EyeProjection {
    FovTangents raw{};
    FovTangents headBounds{};
    Mat3 eyeToHead = Identity3();
};

struct alignas(16) ShaderParams {
    std::array<float, 4> leftFov{};
    std::array<float, 4> rightFov{};
    std::array<float, 4> outputFov{};
    std::array<float, 4> settings{}; // x=feather, y=contrast, z=mode, w=seam center
    std::array<float, 4> leftMap0{};
    std::array<float, 4> leftMap1{};
    std::array<float, 4> leftMap2{};
    std::array<float, 4> rightMap0{};
    std::array<float, 4> rightMap1{};
    std::array<float, 4> rightMap2{};
};



std::filesystem::path ExeDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(len);
    return std::filesystem::path(path).parent_path();
}

class Logger {
public:
    Logger() {
        file_.open(ExeDirectory() / L"VRFusion.log", std::ios::out | std::ios::trunc);
    }

    template <typename T>
    Logger& operator<<(const T& value) {
        if (file_) {
            file_ << value;
            file_.flush();
        }
        return *this;
    }

private:
    std::ofstream file_;
};

Logger gLog;

std::string Trim(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

ViewMode ParseMode(const std::string& value, ViewMode fallback) {
    const std::string v = Lower(Trim(value));
    if (v == "fusion") return ViewMode::Fusion;
    if (v == "left") return ViewMode::Left;
    if (v == "right") return ViewMode::Right;
    if (v == "sbs" || v == "side_by_side" || v == "side-by-side") return ViewMode::SideBySide;
    return fallback;
}

Config LoadConfig() {
    Config cfg;
    std::ifstream in(ExeDirectory() / L"vrfusion.ini");
    if (!in) {
        gLog << "vrfusion.ini not found; using built-in defaults.\n";
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        try {
            if (key == "width") cfg.width = std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(value)), 640, 7680);
            else if (key == "height") cfg.height = std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(value)), 360, 4320);
            else if (key == "fps") cfg.fps = std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(value)), 24, 240);
            else if (key == "feather") cfg.feather = std::clamp(std::stof(value), 0.0f, 0.25f);
            else if (key == "seam_contrast") cfg.seamContrast = std::clamp(std::stof(value), 0.0f, 20.0f);
            else if (key == "zoom") cfg.zoom = std::clamp(std::stof(value), 1.0f, 2.0f);
            else if (key == "stabilization") cfg.stabilization = std::stoi(value) != 0;
            else if (key == "stabilization_ms") cfg.stabilizationMs = std::clamp(std::stof(value), 0.0f, 500.0f);
            else if (key == "max_stabilization_deg") cfg.maxStabilizationDeg = std::clamp(std::stof(value), 0.0f, 30.0f);
            else if (key == "show_fps") cfg.showFps = std::stoi(value) != 0;
            else if (key == "vsync") cfg.vsync = std::stoi(value) != 0;
            else if (key == "mode") cfg.mode = ParseMode(value, cfg.mode);
        } catch (...) {
            gLog << "Ignored invalid config value: " << key << "=" << value << "\n";
        }
    }
    return cfg;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(chars, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), chars);
    return out;
}

std::string HrHex(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return oss.str();
}

void ShowFatal(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"VRFusion - Error", MB_OK | MB_ICONERROR);
}

FovTangents HeadBoundsFromEye(const EyeProjection& eye) {
    FovTangents out{};
    out.left = std::numeric_limits<float>::max();
    out.right = -std::numeric_limits<float>::max();
    out.top = std::numeric_limits<float>::max();
    out.bottom = -std::numeric_limits<float>::max();

    const float xs[2] = {eye.raw.left, eye.raw.right};
    const float ys[2] = {eye.raw.top, eye.raw.bottom};
    for (float tx : xs) {
        for (float ty : ys) {
            // OpenVR camera convention: +X right, +Y up, -Z forward.
            // ProjectionRaw vertical tangent is y/z, so eye ray y is -ty at z=-1.
            const Vec3 head = Transform(eye.eyeToHead, {tx, -ty, -1.0f});
            if (head.z >= -1e-4f) continue;
            const float hx = head.x / -head.z;
            const float hy = head.y / head.z;
            out.left = std::min(out.left, hx);
            out.right = std::max(out.right, hx);
            out.top = std::min(out.top, hy);
            out.bottom = std::max(out.bottom, hy);
        }
    }
    return out;
}

FovTangents AspectCrop(FovTangents fov, float aspect, float zoom) {
    const float cx = 0.5f * (fov.left + fov.right);
    const float cy = 0.5f * (fov.top + fov.bottom);
    float halfW = std::max(0.001f, 0.5f * (fov.right - fov.left));
    float halfH = std::max(0.001f, 0.5f * (fov.bottom - fov.top));

    const float availableAspect = halfW / halfH;
    if (availableAspect > aspect) halfW = halfH * aspect;
    else halfH = halfW / aspect;

    const float invZoom = 1.0f / std::max(1.0f, zoom);
    halfW *= invZoom;
    halfH *= invZoom;
    return {cx-halfW, cx+halfW, cy-halfH, cy+halfH};
}

class VRFusionApp {
public:
    bool Initialize(HINSTANCE instance) {
        config_ = LoadConfig();
        mode_ = config_.mode;
        instance_ = instance;

        if (!InitializeOpenVR()) return false;
        if (!ReadEyeGeometry()) return false;
        if (!CreateWindowAndDevice()) return false;
        if (!CreatePipeline()) return false;
        if (!CreateOutputTexture()) return false;
        if (!AcquireMirrorTextures()) return false;

        RecalculateOutputFov();
        PublishSharedTexture();
        lastPoseTime_ = std::chrono::steady_clock::now();

        gLog << "VRFusion 0.5 initialized: " << config_.width << "x" << config_.height
             << " @ " << config_.fps << " FPS, zoom=" << config_.zoom
             << ", stabilization=" << (config_.stabilization ? 1 : 0) << "\n";
        return true;
    }

    int Run() {
        using clock = std::chrono::steady_clock;
        auto nextFrame = clock::now();
        auto statsStart = nextFrame;
        uint32_t statsFrames = 0;

        MSG msg{};
        while (running_) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running_ = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running_) break;

            const auto frameInterval = std::chrono::duration<double>(1.0 / static_cast<double>(std::max(1u, config_.fps)));
            const auto nowBefore = clock::now();
            if (nowBefore < nextFrame) std::this_thread::sleep_until(nextFrame);

            if (frameLatencyWaitable_) WaitForSingleObject(frameLatencyWaitable_, 1000);

            if (!vr::VRCompositor()) {
                SetWindowTextW(hwnd_, L"VRFusion Spectator - SteamVR compositor unavailable");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }

            UpdateTracking();
            Render();
            ++statsFrames;

            const auto now = clock::now();
            if (config_.showFps && now - statsStart >= std::chrono::seconds(1)) {
                const double elapsed = std::chrono::duration<double>(now - statsStart).count();
                measuredFps_ = static_cast<double>(statsFrames) / elapsed;
                UpdateWindowTitle();
                statsStart = now;
                statsFrames = 0;
            }

            nextFrame += std::chrono::duration_cast<clock::duration>(frameInterval);
            if (now - nextFrame > std::chrono::milliseconds(250)) nextFrame = now;
        }
        return 0;
    }

    ~VRFusionApp() {
        if (vr::VRCompositor()) {
            if (leftMirror_) vr::VRCompositor()->ReleaseMirrorTextureD3D11(leftMirror_.Get());
            if (rightMirror_) vr::VRCompositor()->ReleaseMirrorTextureD3D11(rightMirror_.Get());
        }
        leftMirror_.Detach();
        rightMirror_.Detach();

        if (sharedInfo_) UnmapViewOfFile(sharedInfo_);
        if (sharedMapping_) CloseHandle(sharedMapping_);
        if (sharedFrameEvent_) CloseHandle(sharedFrameEvent_);

        if (vrSystem_) {
            vr::VR_Shutdown();
            vrSystem_ = nullptr;
        }
    }

private:
    bool InitializeOpenVR() {
        vr::EVRInitError error = vr::VRInitError_None;
        vrSystem_ = vr::VR_Init(&error, vr::VRApplication_Background);
        if (error != vr::VRInitError_None || !vrSystem_) {
            const char* text = vr::VR_GetVRInitErrorAsEnglishDescription(error);
            const std::wstring message = L"SteamVR/OpenVR initialization failed.\n\n" + Utf8ToWide(text ? text : "Unknown OpenVR error") +
                                         L"\n\nStart SteamVR, connect the headset, then launch VRFusion again.";
            gLog << "VR_Init failed: " << (text ? text : "unknown") << "\n";
            ShowFatal(message);
            return false;
        }
        if (!vr::VRCompositor()) {
            ShowFatal(L"SteamVR compositor is not available.");
            gLog << "VRCompositor() returned null.\n";
            return false;
        }
        return true;
    }

    bool ReadEyeGeometry() {
        auto read = [&](vr::EVREye side, EyeProjection& out) {
            vrSystem_->GetProjectionRaw(side, &out.raw.left, &out.raw.right, &out.raw.top, &out.raw.bottom);
            const vr::HmdMatrix34_t eyeToHead = vrSystem_->GetEyeToHeadTransform(side);
            out.eyeToHead = RotationOf(eyeToHead);
            out.headBounds = HeadBoundsFromEye(out);
            return eyeToHead.m[0][3];
        };

        const float leftX = read(vr::Eye_Left, leftEye_);
        const float rightX = read(vr::Eye_Right, rightEye_);
        const float ipdMeters = std::fabs(rightX - leftX);

        gLog << "IPD from eye transforms: " << (ipdMeters * 1000.0f) << " mm\n";
        LogEye("Left", leftEye_);
        LogEye("Right", rightEye_);
        return true;
    }

    void LogEye(const char* name, const EyeProjection& e) {
        gLog << name << " raw FOV: " << e.raw.left << ", " << e.raw.right << ", "
             << e.raw.top << ", " << e.raw.bottom << "\n";
        gLog << name << " head bounds: " << e.headBounds.left << ", " << e.headBounds.right << ", "
             << e.headBounds.top << ", " << e.headBounds.bottom << "\n";
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<VRFusionApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_KEYDOWN:
                if (self) self->OnKeyDown(static_cast<UINT>(wp));
                return 0;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                if (self) self->running_ = false;
                PostQuitMessage(0);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            default:
                return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void OnKeyDown(UINT key) {
        switch (key) {
            case VK_F1: SetMode(ViewMode::Fusion); break;
            case VK_F2: SetMode(ViewMode::Left); break;
            case VK_F3: SetMode(ViewMode::Right); break;
            case VK_F4: SetMode(ViewMode::SideBySide); break;
            case VK_F11: ToggleFullscreen(); break;
            case 'S':
                config_.stabilization = !config_.stabilization;
                trackingInitialized_ = false;
                UpdateWindowTitle();
                break;
            case 'R': ReloadRuntimeConfig(); break;
            case VK_OEM_4: // [
                config_.feather = std::max(0.0f, config_.feather - 0.005f);
                UpdateWindowTitle();
                break;
            case VK_OEM_6: // ]
                config_.feather = std::min(0.25f, config_.feather + 0.005f);
                UpdateWindowTitle();
                break;
            case VK_OEM_MINUS:
            case VK_SUBTRACT:
                config_.zoom = std::max(1.0f, config_.zoom - 0.02f);
                RecalculateOutputFov();
                UpdateWindowTitle();
                break;
            case VK_OEM_PLUS:
            case VK_ADD:
                config_.zoom = std::min(2.0f, config_.zoom + 0.02f);
                RecalculateOutputFov();
                UpdateWindowTitle();
                break;
            default: break;
        }
    }

    void SetMode(ViewMode mode) {
        mode_ = mode;
        RecalculateOutputFov();
        UpdateWindowTitle();
    }

    void ReloadRuntimeConfig() {
        const Config reloaded = LoadConfig();
        if (reloaded.width != config_.width || reloaded.height != config_.height) {
            gLog << "R reload: width/height changes require restart; keeping current output size.\n";
        }
        config_.fps = reloaded.fps;
        config_.feather = reloaded.feather;
        config_.seamContrast = reloaded.seamContrast;
        config_.zoom = reloaded.zoom;
        config_.stabilization = reloaded.stabilization;
        config_.stabilizationMs = reloaded.stabilizationMs;
        config_.maxStabilizationDeg = reloaded.maxStabilizationDeg;
        config_.showFps = reloaded.showFps;
        config_.vsync = reloaded.vsync;
        mode_ = reloaded.mode;
        trackingInitialized_ = false;
        RecalculateOutputFov();
        UpdateWindowTitle();
        gLog << "Runtime config reloaded.\n";
    }

    void ToggleFullscreen() {
        if (!fullscreen_) {
            windowedStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
            GetWindowRect(hwnd_, &windowedRect_);
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
                SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(hwnd_, HWND_TOP,
                    mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left,
                    mi.rcMonitor.bottom - mi.rcMonitor.top,
                    SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                fullscreen_ = true;
            }
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, windowedStyle_);
            SetWindowPos(hwnd_, HWND_NOTOPMOST,
                windowedRect_.left, windowedRect_.top,
                windowedRect_.right - windowedRect_.left,
                windowedRect_.bottom - windowedRect_.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            fullscreen_ = false;
        }
    }

    bool CreateWindowAndDevice() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &VRFusionApp::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ShowFatal(L"Could not register the VRFusion window class.");
            return false;
        }

        RECT rect{0, 0, static_cast<LONG>(config_.width), static_cast<LONG>(config_.height)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&rect, style, FALSE);
        hwnd_ = CreateWindowExW(
            0, kWindowClass, kWindowTitle, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance_, this);
        if (!hwnd_) {
            ShowFatal(L"Could not create the VRFusion preview window.");
            return false;
        }

        ComPtr<IDXGIFactory1> factory1;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory1));
        if (FAILED(hr)) {
            ShowFatal(L"CreateDXGIFactory1 failed: " + Utf8ToWide(HrHex(hr)));
            return false;
        }

        int32_t adapterIndex = -1;
        vrSystem_->GetDXGIOutputInfo(&adapterIndex);
        if (adapterIndex < 0) {
            ShowFatal(L"SteamVR did not provide a valid DXGI adapter index.");
            gLog << "GetDXGIOutputInfo returned " << adapterIndex << "\n";
            return false;
        }

        hr = factory1->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter_);
        if (FAILED(hr)) {
            ShowFatal(L"Could not open the GPU adapter selected by SteamVR. HRESULT " + Utf8ToWide(HrHex(hr)));
            return false;
        }

        adapter_->GetDesc1(&adapterDesc_);
        gLog << "SteamVR DXGI adapter index: " << adapterIndex << "\n";

        constexpr D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL created{};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        hr = D3D11CreateDevice(
            adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            requested, static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
            &device_, &created, &context_);
        if (FAILED(hr)) {
            ShowFatal(L"D3D11CreateDevice failed on the SteamVR GPU. HRESULT " + Utf8ToWide(HrHex(hr)));
            return false;
        }

        ComPtr<IDXGIFactory2> factory2;
        hr = factory1.As(&factory2);
        if (FAILED(hr)) {
            ShowFatal(L"DXGI 1.2 is required for the low-latency preview swap chain.");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = config_.width;
        desc.Height = config_.height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        ComPtr<IDXGISwapChain1> swap1;
        hr = factory2->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swap1);
        if (FAILED(hr)) {
            ShowFatal(L"CreateSwapChainForHwnd failed. HRESULT " + Utf8ToWide(HrHex(hr)));
            return false;
        }
        hr = swap1.As(&swapChain_);
        if (FAILED(hr)) return false;
        swapChain_->SetMaximumFrameLatency(1);
        frameLatencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();
        factory1->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

        hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_));
        if (FAILED(hr)) {
            ShowFatal(L"Could not access the preview back buffer.");
            return false;
        }

        const bool headless = wcsstr(GetCommandLineW(), L"--headless") != nullptr;
        ShowWindow(hwnd_, headless ? SW_HIDE : SW_SHOW);
        if (!headless) UpdateWindow(hwnd_);
        return true;
    }

    bool CompileShader(const char* source, const char* entry, const char* profile, ID3DBlob** blob) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(
            source, std::strlen(source), "VRFusionShader", nullptr, nullptr,
            entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
        if (FAILED(hr)) {
            std::string errorText = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Unknown shader compilation error";
            gLog << "Shader compile failed: " << errorText << "\n";
            ShowFatal(L"VRFusion shader compilation failed. See VRFusion.log.");
            return false;
        }
        return true;
    }

    bool CreatePipeline() {
        static constexpr char kShader[] = R"HLSL(
cbuffer Params : register(b0)
{
    float4 leftFov;
    float4 rightFov;
    float4 outputFov;
    float4 settings; // feather, contrast, mode, seamCenter
    float4 leftMap0;
    float4 leftMap1;
    float4 leftMap2;
    float4 rightMap0;
    float4 rightMap1;
    float4 rightMap2;
};

Texture2D leftEye  : register(t0);
Texture2D rightEye : register(t1);
SamplerState linearSampler : register(s0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    VSOut o;
    float2 p;
    if (vertexId == 0) p = float2(-1.0, -1.0);
    else if (vertexId == 1) p = float2(-1.0, 3.0);
    else p = float2(3.0, -1.0);
    o.position = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, 1.0 - ((p.y + 1.0) * 0.5));
    return o;
}

float3 ApplyRows(float3 v, float4 r0, float4 r1, float4 r2)
{
    return float3(dot(v, r0.xyz), dot(v, r1.xyz), dot(v, r2.xyz));
}

bool EyeUv(float3 eyeRay, float4 fov, out float2 uv)
{
    if (eyeRay.z >= -0.0001) {
        uv = 0.0;
        return false;
    }
    const float tanX = eyeRay.x / -eyeRay.z;
    const float tanY = eyeRay.y / eyeRay.z;
    uv = float2(
        (tanX - fov.x) / max(0.00001, fov.y - fov.x),
        (tanY - fov.z) / max(0.00001, fov.w - fov.z));
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    const int mode = (int)(settings.z + 0.5);

    if (mode == 3) {
        if (input.uv.x < 0.5) {
            return leftEye.Sample(linearSampler, float2(input.uv.x * 2.0, input.uv.y));
        }
        return rightEye.Sample(linearSampler, float2((input.uv.x - 0.5) * 2.0, input.uv.y));
    }

    const float tanX = lerp(outputFov.x, outputFov.y, saturate(input.uv.x));
    const float tanY = lerp(outputFov.z, outputFov.w, saturate(input.uv.y));
    const float3 virtualRay = float3(tanX, -tanY, -1.0);

    const float3 rayL = ApplyRows(virtualRay, leftMap0, leftMap1, leftMap2);
    const float3 rayR = ApplyRows(virtualRay, rightMap0, rightMap1, rightMap2);
    float2 uvL, uvR;
    const bool validL = EyeUv(rayL, leftFov, uvL);
    const bool validR = EyeUv(rayR, rightFov, uvR);

    if (mode == 1) return validL ? leftEye.Sample(linearSampler, uvL) : float4(0,0,0,1);
    if (mode == 2) return validR ? rightEye.Sample(linearSampler, uvR) : float4(0,0,0,1);

    if (!validL && !validR) return float4(0, 0, 0, 1);
    if (validL && !validR) return leftEye.Sample(linearSampler, uvL);
    if (!validL && validR) return rightEye.Sample(linearSampler, uvR);

    const float4 a = leftEye.Sample(linearSampler, uvL);
    const float4 b = rightEye.Sample(linearSampler, uvR);

    // Contrast-aware seam: large stereo disagreement means likely close geometry.
    // In those pixels the transition hardens instead of producing a double image.
    const float rgbDiff = dot(abs(a.rgb - b.rgb), float3(0.433333, 0.433333, 0.433333));
    const float hardness = saturate(rgbDiff * settings.y);
    const float baseFeather = max(settings.x, 0.00001);
    const float localFeather = lerp(baseFeather, max(baseFeather * 0.12, 0.0005), hardness);
    const float wRight = smoothstep(settings.w - localFeather, settings.w + localFeather, tanX);
    return lerp(a, b, wRight);
}
)HLSL";

        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        if (!CompileShader(kShader, "VSMain", "vs_5_0", &vsBlob)) return false;
        if (!CompileShader(kShader, "PSMain", "ps_5_0", &psBlob)) return false;

        HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader_);
        if (FAILED(hr)) return false;
        hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader_);
        if (FAILED(hr)) return false;

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device_->CreateSamplerState(&sampler, &sampler_);
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = static_cast<UINT>((sizeof(ShaderParams) + 15u) & ~15u);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(&cbd, nullptr, &constantBuffer_);
        if (FAILED(hr)) return false;

        return true;
    }

    bool CreateOutputTexture() {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = config_.width;
        desc.Height = config_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &outputTexture_);
        if (FAILED(hr)) {
            ShowFatal(L"Could not create the compositor output texture. HRESULT " + Utf8ToWide(HrHex(hr)));
            return false;
        }
        hr = device_->CreateRenderTargetView(outputTexture_.Get(), nullptr, &outputRtv_);
        if (FAILED(hr)) return false;

        // Keep inter-process synchronization away from the preview render target.
        // If no consumer is attached, the producer can continue rendering normally.
        D3D11_TEXTURE2D_DESC sharedDesc = desc;
        sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = device_->CreateTexture2D(&sharedDesc, nullptr, &sharedTexture_);
        if (FAILED(hr)) {
            ShowFatal(L"Could not create the shared D3D11 output texture. HRESULT " + Utf8ToWide(HrHex(hr)));
            return false;
        }

        ComPtr<IDXGIResource> resource;
        hr = sharedTexture_.As(&resource);
        if (FAILED(hr)) return false;
        hr = resource->GetSharedHandle(&sharedTextureHandle_);
        if (FAILED(hr) || !sharedTextureHandle_) {
            ShowFatal(L"Could not obtain a D3D11 shared-texture handle.");
            return false;
        }
        hr = sharedTexture_.As(&sharedKeyedMutex_);
        if (FAILED(hr) || !sharedKeyedMutex_) {
            ShowFatal(L"Could not create the shared-texture synchronization mutex.");
            return false;
        }
        return true;
    }

    void PublishSharedTexture() {
        sharedMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            static_cast<DWORD>(sizeof(vrfusion::SharedFrameInfo)), vrfusion::kSharedMapName);
        if (!sharedMapping_) {
            gLog << "Warning: CreateFileMapping for shared output failed. Win32=" << GetLastError() << "\n";
            return;
        }
        sharedInfo_ = reinterpret_cast<vrfusion::SharedFrameInfo*>(MapViewOfFile(sharedMapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrfusion::SharedFrameInfo)));
        if (!sharedInfo_) {
            gLog << "Warning: MapViewOfFile for shared output failed. Win32=" << GetLastError() << "\n";
            CloseHandle(sharedMapping_);
            sharedMapping_ = nullptr;
            return;
        }

        vrfusion::SharedFrameInfo info{};
        info.width = config_.width;
        info.height = config_.height;
        info.dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
        info.producerPid = GetCurrentProcessId();
        info.adapterLuidLow = adapterDesc_.AdapterLuid.LowPart;
        info.adapterLuidHigh = adapterDesc_.AdapterLuid.HighPart;
        info.sharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sharedTextureHandle_));
        LARGE_INTEGER qpf{};
        if (QueryPerformanceFrequency(&qpf)) info.qpcFrequency = qpf.QuadPart;
        info.flags = vrfusion::SharedFrame_ProducerAlive | vrfusion::SharedFrame_SteamVR;
        std::memcpy(sharedInfo_, &info, sizeof(info));

        sharedFrameEvent_ = CreateEventW(nullptr, FALSE, FALSE, vrfusion::kSharedEventName);
        if (!sharedFrameEvent_) gLog << "Warning: CreateEvent for shared output failed. Win32=" << GetLastError() << "\n";
        else gLog << "Shared GPU output published via " << "Local\\\\VRFusionSharedTexture_v2" << "\n";
    }

    bool AcquireMirrorTextures() {
        void* left = nullptr;
        void* right = nullptr;
        const auto leftErr = vr::VRCompositor()->GetMirrorTextureD3D11(vr::Eye_Left, device_.Get(), &left);
        const auto rightErr = vr::VRCompositor()->GetMirrorTextureD3D11(vr::Eye_Right, device_.Get(), &right);
        if (leftErr != vr::VRCompositorError_None || rightErr != vr::VRCompositorError_None || !left || !right) {
            std::wostringstream message;
            message << L"SteamVR did not provide both eye mirror textures.\n\n"
                    << L"Left error: " << static_cast<int>(leftErr) << L"\n"
                    << L"Right error: " << static_cast<int>(rightErr) << L"\n\n"
                    << L"Make sure a headset is connected and SteamVR is fully running.";
            ShowFatal(message.str());
            gLog << "GetMirrorTextureD3D11 failed: left=" << static_cast<int>(leftErr)
                 << ", right=" << static_cast<int>(rightErr) << "\n";
            return false;
        }

        leftMirror_.Attach(reinterpret_cast<ID3D11ShaderResourceView*>(left));
        rightMirror_.Attach(reinterpret_cast<ID3D11ShaderResourceView*>(right));

        LogMirrorTexture("Left", leftMirror_.Get());
        LogMirrorTexture("Right", rightMirror_.Get());
        return true;
    }

    void LogMirrorTexture(const char* name, ID3D11ShaderResourceView* srv) {
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            gLog << name << " mirror texture: " << desc.Width << "x" << desc.Height
                 << " format=" << static_cast<int>(desc.Format) << "\n";
        }
    }

    void RecalculateOutputFov() {
        FovTangents source{};
        if (mode_ == ViewMode::Left) source = leftEye_.headBounds;
        else if (mode_ == ViewMode::Right) source = rightEye_.headBounds;
        else {
            source.left = std::min(leftEye_.headBounds.left, rightEye_.headBounds.left);
            source.right = std::max(leftEye_.headBounds.right, rightEye_.headBounds.right);
            source.top = std::min(leftEye_.headBounds.top, rightEye_.headBounds.top);
            source.bottom = std::max(leftEye_.headBounds.bottom, rightEye_.headBounds.bottom);
        }

        const float aspect = static_cast<float>(config_.width) / static_cast<float>(config_.height);
        outputFov_ = AspectCrop(source, aspect, config_.zoom);

        const float overlapLeft = std::max(leftEye_.headBounds.left, rightEye_.headBounds.left);
        const float overlapRight = std::min(leftEye_.headBounds.right, rightEye_.headBounds.right);
        seamCenter_ = overlapLeft < overlapRight ? 0.5f * (overlapLeft + overlapRight) : 0.0f;

        gLog << "Output FOV tangents: " << outputFov_.left << ", " << outputFov_.right << ", "
             << outputFov_.top << ", " << outputFov_.bottom << ", seam=" << seamCenter_ << "\n";
    }

    void UpdateTracking() {
        vr::TrackedDevicePose_t pose{};
        const auto error = vr::VRCompositor()->GetLastPoseForTrackedDeviceIndex(
            vr::k_unTrackedDeviceIndex_Hmd, &pose, nullptr);
        if (error != vr::VRCompositorError_None || !pose.bPoseIsValid) {
            stabilizationMap_ = Identity3();
            trackingInitialized_ = false;
            return;
        }

        const Mat3 currentWorldFromHead = RotationOf(pose.mDeviceToAbsoluteTracking);
        const Quat currentQ = QuatFromMat3(currentWorldFromHead);
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::max(0.0001f, std::chrono::duration<float>(now - lastPoseTime_).count());
        lastPoseTime_ = now;

        if (!trackingInitialized_ || !config_.stabilization || config_.stabilizationMs <= 0.0f) {
            smoothedOrientation_ = currentQ;
            trackingInitialized_ = true;
        } else {
            const float tau = std::max(0.001f, config_.stabilizationMs / 1000.0f);
            float alpha = 1.0f - std::exp(-dt / tau);

            const float angle = AngularDistance(smoothedOrientation_, currentQ);
            const float maxAngle = config_.maxStabilizationDeg * (kPi / 180.0f);
            if (maxAngle > 0.0f && angle > maxAngle) {
                const float clampAlpha = 1.0f - (maxAngle / angle);
                alpha = std::max(alpha, clampAlpha);
            }
            smoothedOrientation_ = Slerp(smoothedOrientation_, currentQ, alpha);
        }

        if (config_.stabilization) {
            const Mat3 smoothWorldFromHead = Mat3FromQuat(smoothedOrientation_);
            stabilizationMap_ = Multiply(Transpose(currentWorldFromHead), smoothWorldFromHead);
        } else {
            stabilizationMap_ = Identity3();
        }
    }

    void FillMatrixRows(const Mat3& m,
                        std::array<float,4>& r0,
                        std::array<float,4>& r1,
                        std::array<float,4>& r2) {
        r0 = {m.m[0][0], m.m[0][1], m.m[0][2], 0.0f};
        r1 = {m.m[1][0], m.m[1][1], m.m[1][2], 0.0f};
        r2 = {m.m[2][0], m.m[2][1], m.m[2][2], 0.0f};
    }

    bool UpdateConstantBuffer() {
        ShaderParams params{};
        params.leftFov = {leftEye_.raw.left, leftEye_.raw.right, leftEye_.raw.top, leftEye_.raw.bottom};
        params.rightFov = {rightEye_.raw.left, rightEye_.raw.right, rightEye_.raw.top, rightEye_.raw.bottom};
        params.outputFov = {outputFov_.left, outputFov_.right, outputFov_.top, outputFov_.bottom};
        params.settings = {config_.feather, config_.seamContrast, static_cast<float>(mode_), seamCenter_};

        // output stabilized-head ray -> current-head ray -> eye-local ray
        const Mat3 leftHeadToEye = Transpose(leftEye_.eyeToHead);
        const Mat3 rightHeadToEye = Transpose(rightEye_.eyeToHead);
        const Mat3 leftMap = Multiply(leftHeadToEye, stabilizationMap_);
        const Mat3 rightMap = Multiply(rightHeadToEye, stabilizationMap_);
        FillMatrixRows(leftMap, params.leftMap0, params.leftMap1, params.leftMap2);
        FillMatrixRows(rightMap, params.rightMap0, params.rightMap1, params.rightMap2);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return false;
        std::memcpy(mapped.pData, &params, sizeof(params));
        context_->Unmap(constantBuffer_.Get(), 0);
        return true;
    }

    void Render() {
        LARGE_INTEGER renderStart{};
        QueryPerformanceCounter(&renderStart);
        ++sourceFrameCounter_;
        if (!UpdateConstantBuffer()) return;

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(outputRtv_.Get(), clear);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(config_.width);
        viewport.Height = static_cast<float>(config_.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        ID3D11RenderTargetView* rtv = outputRtv_.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {leftMirror_.Get(), rightMirror_.Get()};
        context_->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer* cb = constantBuffer_.Get();
        context_->PSSetConstantBuffers(0, 1, &cb);

        context_->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, nullSrvs);
        context_->OMSetRenderTargets(0, nullptr, nullptr);

        context_->CopyResource(backBuffer_.Get(), outputTexture_.Get());

        // Key 0 belongs to the producer, key 1 to the consumer. If nobody is
        // consuming the shared texture, AcquireSync simply fails after the first
        // published frame and the preview keeps running without blocking.
        bool published = false;
        if (sharedKeyedMutex_ && sharedKeyedMutex_->AcquireSync(0, 0) == S_OK) {
            context_->CopyResource(sharedTexture_.Get(), outputTexture_.Get());
            context_->Flush();
            sharedKeyedMutex_->ReleaseSync(1);
            ++sharedFrameCounter_;
            published = true;
        } else if (sharedKeyedMutex_) {
            ++droppedPublishFrames_;
        }

        LARGE_INTEGER renderEnd{};
        QueryPerformanceCounter(&renderEnd);
        if (sharedInfo_) {
            InterlockedExchange64(&sharedInfo_->sourceFrameCounter, static_cast<LONG64>(sourceFrameCounter_));
            InterlockedExchange64(&sharedInfo_->producerQpc, renderEnd.QuadPart);
            InterlockedExchange64(&sharedInfo_->renderDurationQpc, renderEnd.QuadPart - renderStart.QuadPart);
            InterlockedExchange64(&sharedInfo_->droppedPublishFrames, static_cast<LONG64>(droppedPublishFrames_));
            if (published) InterlockedExchange64(&sharedInfo_->frameCounter, static_cast<LONG64>(sharedFrameCounter_));
        }
        if (published && sharedFrameEvent_) SetEvent(sharedFrameEvent_);

        const HRESULT hr = swapChain_->Present(config_.vsync ? 1u : 0u, 0);
        if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) {
            gLog << "Present failed: " << HrHex(hr) << "\n";
        }
    }

    void UpdateWindowTitle() {
        std::wostringstream title;
        title << L"VRFusion 0.5  |  " << ViewModeName(mode_)
              << L"  |  " << (config_.stabilization ? L"Stabilized" : L"Raw motion")
              << L"  |  zoom " << std::fixed << std::setprecision(2) << config_.zoom;
        if (config_.showFps) title << L"  |  " << std::setprecision(1) << measuredFps_ << L" FPS";
        if (sharedInfo_ && sharedInfo_->qpcFrequency > 0) {
            const double renderMs = 1000.0 * static_cast<double>(sharedInfo_->renderDurationQpc) / static_cast<double>(sharedInfo_->qpcFrequency);
            title << L"  |  " << std::setprecision(2) << renderMs << L" ms"
                  << L"  |  pubdrop " << sharedInfo_->droppedPublishFrames;
        }
        SetWindowTextW(hwnd_, title.str().c_str());
    }

private:
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool running_ = true;
    bool fullscreen_ = false;
    DWORD windowedStyle_ = 0;
    RECT windowedRect_{};
    Config config_{};
    ViewMode mode_ = ViewMode::Fusion;
    double measuredFps_ = 0.0;

    vr::IVRSystem* vrSystem_ = nullptr;
    EyeProjection leftEye_{};
    EyeProjection rightEye_{};
    FovTangents outputFov_{};
    float seamCenter_ = 0.0f;

    bool trackingInitialized_ = false;
    Quat smoothedOrientation_{};
    Mat3 stabilizationMap_ = Identity3();
    std::chrono::steady_clock::time_point lastPoseTime_{};

    ComPtr<IDXGIAdapter1> adapter_;
    DXGI_ADAPTER_DESC1 adapterDesc_{};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain2> swapChain_;
    HANDLE frameLatencyWaitable_ = nullptr; // owned by swap chain
    ComPtr<ID3D11Texture2D> backBuffer_;

    ComPtr<ID3D11Texture2D> outputTexture_;
    ComPtr<ID3D11RenderTargetView> outputRtv_;
    ComPtr<ID3D11Texture2D> sharedTexture_;
    ComPtr<IDXGIKeyedMutex> sharedKeyedMutex_;
    HANDLE sharedTextureHandle_ = nullptr;
    HANDLE sharedMapping_ = nullptr;
    vrfusion::SharedFrameInfo* sharedInfo_ = nullptr;
    HANDLE sharedFrameEvent_ = nullptr;
    uint64_t sharedFrameCounter_ = 0;
    uint64_t sourceFrameCounter_ = 0;
    uint64_t droppedPublishFrames_ = 0;

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11ShaderResourceView> leftMirror_;
    ComPtr<ID3D11ShaderResourceView> rightMirror_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    VRFusionApp app;
    if (!app.Initialize(instance)) return 1;
    return app.Run();
}
