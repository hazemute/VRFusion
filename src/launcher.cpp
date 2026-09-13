#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {
std::filesystem::path ExeDir() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return n ? std::filesystem::path(std::wstring(buf.data(), n)).parent_path() : std::filesystem::current_path();
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto exe = ExeDir() / L"VRFusion.exe";
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
        MessageBoxW(nullptr, L"VRFusion.exe was not found next to VRFusionLauncher.exe.", L"VRFusion 0.5", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
