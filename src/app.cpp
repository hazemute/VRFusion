#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "control_protocol.hpp"
#include "openxr_capture_protocol.hpp"
#include "shared_protocol.hpp"
#include "stable_output.hpp"

namespace {

constexpr wchar_t kWindowClass[] = L"VRFusionControllerWindow";
constexpr wchar_t kWindowTitle[] = L"VRFusion 0.5";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 16;

enum class Backend { Waiting, SteamVR, OpenXR };

std::filesystem::path ExeDir() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return n ? std::filesystem::path(std::wstring(buf.data(), n)).parent_path() : std::filesystem::current_path();
}

bool IsProcessRunning(DWORD pid) {
    if (!pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

bool ProcessRunning(const wchar_t* wanted) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    if (Process32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szExeFile, wanted) == 0) { found = true; break; }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

bool EnsureLayerRegistered(std::wstring& error) {
    const auto dir = ExeDir();
    const auto manifest = std::filesystem::absolute(dir / L"XR_APILAYER_VRFusion_capture.json");
    const auto dll = dir / L"VRFusionOpenXRLayer.dll";
    if (!std::filesystem::exists(manifest)) {
        error = L"Missing OpenXR manifest: " + manifest.wstring();
        return false;
    }
    if (!std::filesystem::exists(dll)) {
        error = L"Missing OpenXR layer DLL: " + dll.wstring();
        return false;
    }

    constexpr wchar_t keyPath[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
    HKEY key{};
    DWORD disposition = 0;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, &disposition);
    if (r != ERROR_SUCCESS) {
        error = L"Could not open the per-user OpenXR layer registry key. Error " + std::to_wstring(r);
        return false;
    }

    // Remove stale VRFusion registrations left behind when the portable folder was moved.
    std::vector<std::wstring> stale;
    for (DWORD index = 0;; ++index) {
        wchar_t name[32768]{};
        DWORD chars = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        r = RegEnumValueW(key, index, name, &chars, nullptr, &type, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        std::wstring valueName(name, chars);
        std::wstring lower = valueName;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lower.find(L"vrfusion") != std::wstring::npos && _wcsicmp(valueName.c_str(), manifest.c_str()) != 0)
            stale.push_back(valueName);
    }
    for (const auto& valueName : stale) RegDeleteValueW(key, valueName.c_str());

    const DWORD enabled = 0;
    r = RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        error = L"Could not register the OpenXR layer. Error " + std::to_wstring(r);
        return false;
    }
    return true;
}

class Controller {
public:
    bool Init(HINSTANCE instance) {
        instance_ = instance;
        setupOk_ = EnsureLayerRegistered(setupError_);
        if (!CreateControlMapping()) {
            setupOk_ = false;
            setupError_ = L"Could not create the VRFusion control channel.";
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WndProcThunk;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = CreateSolidBrush(RGB(18, 20, 24));
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        RECT rect{0, 0, 650, 360};
        AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;

        CreateWindowExW(0, L"BUTTON", L"Diagnostics",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        28, 292, 130, 34, hwnd_, reinterpret_cast<HMENU>(1001), instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Show output",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        170, 292, 140, 34, hwnd_, reinterpret_cast<HMENU>(1002), instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Restart capture",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        322, 292, 140, 34, hwnd_, reinterpret_cast<HMENU>(1003), instance_, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Exit",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        474, 292, 140, 34, hwnd_, reinterpret_cast<HMENU>(1004), instance_, nullptr);

        if (!output_.Initialize(instance_)) {
            setupOk_ = false;
            setupError_ = L"Could not initialize the stable VRFusion Output window.";
        }

        SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        Tick();
        return true;
    }

    int Run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    ~Controller() {
        StopChild();
        ShutdownControlMapping();
    }

private:
    bool CreateControlMapping() {
        controlMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(sizeof(vrfusion::control::SharedControlInfo)),
                                         vrfusion::control::kControlMapName);
        if (!controlMap_) return false;
        control_ = reinterpret_cast<vrfusion::control::SharedControlInfo*>(
            MapViewOfFile(controlMap_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrfusion::control::SharedControlInfo)));
        if (!control_) {
            CloseHandle(controlMap_);
            controlMap_ = nullptr;
            return false;
        }
        *control_ = {};
        control_->magic = vrfusion::control::kControlMagic;
        control_->version = vrfusion::control::kControlVersion;
        control_->controllerPid = GetCurrentProcessId();
        control_->flags = vrfusion::control::Control_Active | vrfusion::control::Control_AutoBackend;
        InterlockedExchange64(&control_->heartbeatTickMs, static_cast<LONG64>(GetTickCount64()));
        InterlockedExchange64(&control_->generation, 1);
        return true;
    }

    void ShutdownControlMapping() {
        if (control_) {
            control_->flags = 0;
            InterlockedExchange64(&control_->heartbeatTickMs, static_cast<LONG64>(GetTickCount64()));
            InterlockedIncrement64(&control_->generation);
            UnmapViewOfFile(control_);
            control_ = nullptr;
        }
        if (controlMap_) {
            CloseHandle(controlMap_);
            controlMap_ = nullptr;
        }
    }

    bool OpenXRReady() {
        HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::xripc::kCaptureMapName);
        if (!map) return false;
        auto* info = reinterpret_cast<const vrfusion::xripc::SharedCaptureInfo*>(
            MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(vrfusion::xripc::SharedCaptureInfo)));
        bool ready = false;
        if (info && info->magic == vrfusion::xripc::kCaptureMagic && info->version == vrfusion::xripc::kCaptureVersion) {
            const bool stereo = (info->flags & (vrfusion::xripc::Capture_LeftColorValid | vrfusion::xripc::Capture_RightColorValid)) ==
                                (vrfusion::xripc::Capture_LeftColorValid | vrfusion::xripc::Capture_RightColorValid);
            ready = stereo && IsProcessRunning(info->producerPid);
            xrDepth_ = ready && (info->flags & vrfusion::xripc::Capture_DepthAwareReady) != 0;
        }
        if (info) UnmapViewOfFile(info);
        CloseHandle(map);
        return ready;
    }


    bool FinalOutputLive() {
        HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, vrfusion::kSharedMapName);
        if (!map) return false;
        auto* info = reinterpret_cast<const vrfusion::SharedFrameInfo*>(
            MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(vrfusion::SharedFrameInfo)));
        bool live = false;
        if (info && info->magic == vrfusion::kSharedMagic && info->version == vrfusion::kSharedVersion &&
            info->frameCounter > 0 && IsProcessRunning(info->producerPid)) {
            if (info->qpcFrequency > 0 && info->producerQpc > 0) {
                LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                const double ageMs = 1000.0 * static_cast<double>(now.QuadPart - info->producerQpc) / static_cast<double>(info->qpcFrequency);
                live = ageMs >= 0.0 && ageMs < 1500.0;
            } else {
                live = true;
            }
        }
        if (info) UnmapViewOfFile(info);
        CloseHandle(map);
        return live;
    }
    Backend DesiredBackend() {
        if (OpenXRReady()) return Backend::OpenXR;
        xrDepth_ = false;
        if (ProcessRunning(L"vrserver.exe")) return Backend::SteamVR;
        return Backend::Waiting;
    }

    bool ChildAlive() const {
        return childProcess_ && WaitForSingleObject(childProcess_, 0) == WAIT_TIMEOUT;
    }

    void StopChild() {
        if (!childProcess_) return;
        if (ChildAlive()) {
            // Ask the preview to close cleanly first, then terminate only if it refuses.
            const DWORD pid = GetProcessId(childProcess_);
            EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
                DWORD windowPid = 0;
                GetWindowThreadProcessId(hwnd, &windowPid);
                if (windowPid == static_cast<DWORD>(param)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            }, static_cast<LPARAM>(pid));
            if (WaitForSingleObject(childProcess_, 1000) == WAIT_TIMEOUT) TerminateProcess(childProcess_, 0);
        }
        CloseHandle(childProcess_);
        childProcess_ = nullptr;
        childBackend_ = Backend::Waiting;
    }

    bool SpawnBackend(Backend backend) {
        const wchar_t* name = backend == Backend::OpenXR ? L"VRFusionXRView.exe" : L"VRFusionSteamVR.exe";
        const auto exe = ExeDir() / name;
        if (!std::filesystem::exists(exe)) {
            setupError_ = L"Missing component: " + exe.wstring();
            setupOk_ = false;
            return false;
        }
        std::wstring cmd = L"\"" + exe.wstring() + L"\" --headless";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
            setupError_ = L"Could not start capture backend. Win32 error " + std::to_wstring(GetLastError());
            setupOk_ = false;
            return false;
        }
        CloseHandle(pi.hThread);
        childProcess_ = pi.hProcess;
        childBackend_ = backend;
        return true;
    }

    void EnsureBackend(Backend desired) {
        if (desired == Backend::Waiting) {
            if (childProcess_ && !ChildAlive()) {
                CloseHandle(childProcess_);
                childProcess_ = nullptr;
                childBackend_ = Backend::Waiting;
            }
            return;
        }
        if (childProcess_ && ChildAlive() && childBackend_ == desired) return;
        StopChild();
        SpawnBackend(desired);
    }

    void Tick() {
        const ULONGLONG now = GetTickCount64();
        if (control_) InterlockedExchange64(&control_->heartbeatTickMs, static_cast<LONG64>(now));
        output_.Render();
        if (now - lastBackendCheckMs_ >= 250) {
            lastBackendCheckMs_ = now;
            desiredBackend_ = DesiredBackend();
            EnsureBackend(desiredBackend_);
            outputLive_ = FinalOutputLive();
            if (outputLive_ && !outputShownOnce_) {
                output_.Show();
                outputShownOnce_ = true;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        HBRUSH bg = CreateSolidBrush(RGB(18, 20, 24));
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        HFONT titleFont = CreateFontW(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT bodyFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT smallFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        SelectObject(dc, titleFont);
        SetTextColor(dc, RGB(245, 247, 250));
        TextOutW(dc, 28, 24, L"VRFusion", 8);

        SelectObject(dc, bodyFont);
        std::wstring status;
        COLORREF statusColor = RGB(145, 151, 163);
        if (!setupOk_) {
            status = L"Setup error";
            statusColor = RGB(235, 95, 95);
        } else if (desiredBackend_ == Backend::Waiting) {
            status = L"READY — start a VR game";
            statusColor = RGB(111, 170, 255);
        } else if (!outputLive_) {
            status = desiredBackend_ == Backend::OpenXR ? L"CONNECTING — OpenXR" : L"CONNECTING — SteamVR";
            statusColor = RGB(240, 190, 95);
        } else if (desiredBackend_ == Backend::OpenXR) {
            status = xrDepth_ ? L"LIVE — OpenXR depth center view" : L"LIVE — OpenXR stereo fusion";
            statusColor = RGB(100, 220, 150);
        } else {
            status = L"LIVE — SteamVR automatic fallback";
            statusColor = RGB(100, 220, 150);
        }
        SetTextColor(dc, statusColor);
        TextOutW(dc, 28, 78, status.c_str(), static_cast<int>(status.size()));

        SelectObject(dc, smallFont);
        SetTextColor(dc, RGB(181, 187, 198));
        const std::wstring setup = setupOk_
            ? L"OpenXR layer: configured automatically  •  Backend: automatic  •  No PowerShell setup required"
            : setupError_;
        RECT setupRect{28, 118, 620, 170};
        DrawTextW(dc, setup.c_str(), -1, &setupRect, DT_LEFT | DT_WORDBREAK);

        const std::wstring streamLine = L"Streaming output: stable VRFusion Output window + shared GPU texture for the OBS source";
        RECT streamRect{28, 174, 620, 218};
        DrawTextW(dc, streamLine.c_str(), -1, &streamRect, DT_LEFT | DT_WORDBREAK);

        SetTextColor(dc, RGB(128, 134, 146));
        const std::wstring hint = L"Normal use: open VRFusion.exe, then start the VR game. VRFusion switches to the best available backend by itself.";
        RECT hintRect{28, 226, 620, 278};
        DrawTextW(dc, hint.c_str(), -1, &hintRect, DT_LEFT | DT_WORDBREAK);

        DeleteObject(titleFont);
        DeleteObject(bodyFont);
        DeleteObject(smallFont);
        EndPaint(hwnd_, &ps);
    }

    void OpenDiagnostics() {
        const auto exe = ExeDir() / L"VRFusionDoctor.exe";
        ShellExecuteW(hwnd_, L"open", exe.c_str(), nullptr, ExeDir().c_str(), SW_SHOWNORMAL);
    }

    void ShowOutput() {
        output_.Show();
    }

    void RestartCapture() {
        StopChild();
        Tick();
    }

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Controller* self = reinterpret_cast<Controller*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Controller*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        }
        return self ? self->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_TIMER:
                if (wParam == kTimerId) Tick();
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wParam)) {
                    case 1001: OpenDiagnostics(); return 0;
                    case 1002: ShowOutput(); return 0;
                    case 1003: RestartCapture(); return 0;
                    case 1004: DestroyWindow(hwnd); return 0;
                }
                break;
            case WM_PAINT:
                Paint();
                return 0;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kTimerId);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE controlMap_ = nullptr;
    vrfusion::control::SharedControlInfo* control_ = nullptr;
    HANDLE childProcess_ = nullptr;
    Backend childBackend_ = Backend::Waiting;
    Backend desiredBackend_ = Backend::Waiting;
    bool setupOk_ = false;
    bool xrDepth_ = false;
    bool outputLive_ = false;
    bool outputShownOnce_ = false;
    ULONGLONG lastBackendCheckMs_ = 0;
    vrfusion::StableOutput output_;
    std::wstring setupError_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\VRFusionControllerSingleton_v1");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleton);
        return 0;
    }

    Controller app;
    if (!app.Init(instance)) {
        MessageBoxW(nullptr, L"VRFusion could not initialize its controller window.", kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    const int result = app.Run();
    if (singleton) CloseHandle(singleton);
    return result;
}
