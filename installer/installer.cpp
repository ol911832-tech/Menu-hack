// Kuang Installer - compiles and installs every Kuang mod for Geometry Dash 2.2091 in one click.
//
//  1. closes Geometry Dash and the Kuang apps if they are open (asks first)
//  2. compiles the source (CMake + Visual Studio Build Tools, the same as compilar.bat)
//  3. copies kfe.dll / kopt.dll / kmenu.dll and the shared loader (winmm.dll) into the game folder
//     and checks byte by byte that the new files really landed
//  4. refreshes the apps in dist\ and can create desktop shortcuts
// Optional "clean build": deletes build\ and dist\ before compiling, so nothing old is left behind.
// Command line (used by ACTUALIZAR.bat): --auto  installs right away, no questions, closes itself when done;
//                                        --from-build  installs what is in build\Release (already compiled).
// Uninstall removes the mods and the loader (your .ini settings are kept).
//
// The built mods are also packed inside KuangInstaller.exe (resources 101-106), so the .exe alone
// is enough to install everything on any PC, without the source code or the build tools.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "../shared/kfe_shared.h"

#define INSTALLER_VERSION "1.1.0"

// ---------------------------------------------------------------------------
// D3D11 boilerplate
// ---------------------------------------------------------------------------
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static void createRT() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}
static void cleanupRT() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &fl, &g_context);
    if (r == DXGI_ERROR_UNSUPPORTED)
        r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &fl, &g_context);
    if (r != S_OK) return false;
    createRT();
    return true;
}
static void cleanupDevice() {
    cleanupRT();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
            return 0;
        case WM_GETMINMAXINFO: {
            auto mm = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = GetDpiForWindow(hwnd);
            mm->ptMinTrackSize.x = MulDiv(560, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(560, dpi, 96);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring withSlash(std::wstring s) {
    if (!s.empty() && s.back() != L'\\' && s.back() != L'/') s += L'\\';
    return s;
}
static std::wstring parentDir(std::wstring d) {
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    size_t p = d.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : d.substr(0, p + 1);
}
static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
static bool isGdFolder(const std::wstring& dir) { return fileExists(withSlash(dir) + L"GeometryDash.exe"); }

static bool readFile(const std::wstring& path, std::string& out) {
    out.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
static bool sameFile(const std::wstring& a, const std::wstring& b) {
    std::string x, y;
    return readFile(a, x) && readFile(b, y) && x == y;
}
static bool fileContains(const std::wstring& path, const char* marker) {
    std::string d;
    return readFile(path, d) && d.find(marker) != std::string::npos;
}

static std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[4096];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) == ERROR_SUCCESS) {
        wchar_t out[8192];
        if (ExpandEnvironmentStringsW(buf, out, 8192)) return out;
        return buf;
    }
    return {};
}

// Steam library folders -> ...\steamapps\common\Geometry Dash
static std::wstring findGdFolder() {
    std::vector<std::wstring> libs;
    std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!steam.empty()) {
        std::replace(steam.begin(), steam.end(), L'/', L'\\');
        libs.push_back(steam);
        std::string text;
        if (readFile(withSlash(steam) + L"steamapps\\libraryfolders.vdf", text)) {
            size_t pos = 0;
            while ((pos = text.find("\"path\"", pos)) != std::string::npos) {
                size_t a = text.find('"', pos + 6);
                size_t b = a == std::string::npos ? a : text.find('"', a + 1);
                if (b == std::string::npos) break;
                std::string p = text.substr(a + 1, b - a - 1), clean;
                for (size_t i = 0; i < p.size(); i++) {
                    if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++;
                    clean += p[i];
                }
                libs.push_back(widen(clean));
                pos = b;
            }
        }
    }
    for (auto& l : libs) {
        std::wstring d = withSlash(l) + L"steamapps\\common\\Geometry Dash";
        if (isGdFolder(d)) return d;
    }
    // installer placed inside the game folder (or in kfe-src inside it)
    for (std::wstring d = exeDir(); !d.empty(); d = parentDir(d))
        if (isGdFolder(d)) return d.substr(0, d.size() - 1);
    return {};
}

// the source folder: where CMakeLists.txt of this project is (installer sits in it, or in dist\ / build\Release\)
static std::wstring findSourceFolder() {
    std::wstring d = exeDir();
    for (int i = 0; i < 4 && !d.empty(); i++, d = parentDir(d)) {
        std::string text;
        if (readFile(d + L"CMakeLists.txt", text) && text.find("KuangFrameExtrapolation") != std::string::npos) return d;
        if (dirExists(d + L"kfe-src")) {
            std::string t2;
            if (readFile(d + L"kfe-src\\CMakeLists.txt", t2)) return d + L"kfe-src\\";
        }
    }
    return {};
}

static std::wstring pickFolder(HWND owner, const wchar_t* title) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(title);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
                item->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

static bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool r = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return r;
}

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------
static const wchar_t* kWatched[] = {L"GeometryDash.exe", L"KuangOptimizer.exe", L"KuangFrameExtrapolation.exe"};

static std::vector<DWORD> findProcesses(const wchar_t* exe) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exe) == 0) out.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

static std::vector<std::wstring> runningApps() {
    std::vector<std::wstring> r;
    for (auto e : kWatched) if (!findProcesses(e).empty()) r.push_back(e);
    return r;
}

static BOOL CALLBACK closeWindowsOf(HWND hwnd, LPARAM pid) {
    DWORD p = 0;
    GetWindowThreadProcessId(hwnd, &p);
    if (p == static_cast<DWORD>(pid)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

// asks nicely (WM_CLOSE, so Geometry Dash saves), then forces it after a while
static bool closeProcess(const wchar_t* exe, int waitMs) {
    auto pids = findProcesses(exe);
    if (pids.empty()) return true;
    for (DWORD pid : pids) EnumWindows(closeWindowsOf, static_cast<LPARAM>(pid));
    for (int t = 0; t < waitMs; t += 100) {
        if (findProcesses(exe).empty()) return true;
        Sleep(100);
    }
    for (DWORD pid : findProcesses(exe)) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
        }
    }
    Sleep(300);
    return findProcesses(exe).empty();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Component {
    const char* name;
    const char* desc;
    const wchar_t* dll;
    const wchar_t* app;   // control app (nullptr for the menu: it lives inside the game)
    bool want = true;
    int state = 0;        // 0 not installed, 1 installed and up to date, 2 installed but older, 3 installed (can't compare)
};

Component g_comps[] = {
    {"Kuang Frame Extrapolation", "Smooth camera, icons and moving objects at any FPS.", L"kfe.dll", L"KuangFrameExtrapolation.exe"},
    {"Kuang Optimizer", "Lower input lag and Windows tweaks while you play.", L"kopt.dll", L"KuangOptimizer.exe"},
    {"Kuang Menu", "The in-game mod menu (opens with Tab): cheats, labels, custom trail...", L"kmenu.dll", nullptr},
};
constexpr int kComps = 3;

struct LogLine { std::string text; int kind; }; // 0 normal, 1 step, 2 ok, 3 warn, 4 error, 5 compiler output

struct App {
    std::wstring gdDir, srcDir, settingsIni;
    std::wstring payloadDir;   // the mods packed inside this .exe, extracted to %TEMP%
    bool installApps = true;   // KFE and Optimizer control programs -> %LOCALAPPDATA%\Programs\Kuang
    bool compile = true;
    bool cleanBuild = true;    // delete build\ and dist\ before compiling
    bool autoMode = false;     // --auto: start installing at once and close when it worked
    bool fromBuild = false;    // --from-build: install build\Release (ACTUALIZAR.bat compiled it already)
    ULONGLONG autoDoneAt = 0;
    bool shortcuts = false;
    bool cmakeFound = false;
    std::wstring cmakePath;
    std::vector<std::wstring> running;
    double lastPoll = -10;
    std::mutex logMutex;
    std::vector<LogLine> log;
    std::atomic<bool> busy{false};
    std::atomic<int> progress{0};   // 0..100
    std::atomic<int> result{0};     // 0 none, 1 ok, 2 failed
    std::string stepName;
    bool confirmClose = false;
    int pendingAction = 0;          // 1 install, 2 uninstall
    bool scrollLog = false;
} app;

static void logLine(int kind, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(app.logMutex);
    app.log.push_back({buf, kind});
    if (app.log.size() > 4000) app.log.erase(app.log.begin(), app.log.begin() + 500);
    app.scrollLog = true;
}
static void setStep(const char* s, int progress) {
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        app.stepName = s;
    }
    app.progress = progress;
    logLine(1, "%s", s);
}

static std::wstring buildDir() { return app.srcDir + L"build\\Release\\"; }
static std::wstring distDir() { return app.srcDir + L"dist\\"; }

// ---- files packed inside the installer
struct Payload { int id; const wchar_t* name; };
const Payload kPayload[] = {
    {101, L"kfe.dll"}, {102, L"kopt.dll"}, {103, L"kmenu.dll"}, {104, L"winmm.dll"},
    {105, L"KuangFrameExtrapolation.exe"}, {106, L"KuangOptimizer.exe"},
};

static std::wstring extractPayload() {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return {};
    std::wstring dir = std::wstring(tmp) + L"KuangInstaller\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    int found = 0;
    for (auto& p : kPayload) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(p.id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
        if (!r) continue;
        HGLOBAL g = LoadResource(nullptr, r);
        DWORD size = SizeofResource(nullptr, r);
        const void* data = g ? LockResource(g) : nullptr;
        if (!data || !size) continue;
        FILE* f = _wfopen((dir + p.name).c_str(), L"wb");
        if (!f) continue;
        fwrite(data, 1, size, f);
        fclose(f);
        found++;
    }
    // the four DLLs are the minimum to install the mods
    for (int i = 0; i < 4; i++) if (!fileExists(dir + kPayload[i].name)) return {};
    return found ? dir : std::wstring();
}

static std::wstring appsDir() {
    wchar_t la[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    return std::wstring(la) + L"\\Programs\\Kuang\\";
}

// where the files to install come from:
//   compile on -> build\Release;  otherwise the copy packed in this .exe;  otherwise whatever was built before
static std::wstring binDir() {
    if (app.fromBuild && !app.srcDir.empty()) return buildDir();
    if (app.compile && !app.srcDir.empty()) return buildDir();
    if (!app.payloadDir.empty()) return app.payloadDir;
    if (!app.srcDir.empty() && fileExists(buildDir() + L"kmenu.dll")) return buildDir();
    if (!app.srcDir.empty() && fileExists(distDir() + L"winmm.dll")) return distDir();
    return exeDir();
}

static std::wstring findCmake() {
    // same as compilar.bat: the PATH saved in the registry + the default CMake folder
    std::wstring path = regString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"Path") +
        L";" + regString(HKEY_CURRENT_USER, L"Environment", L"Path");
    wchar_t pf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    path += L";" + std::wstring(pf) + L"\\CMake\\bin";
    size_t a = 0;
    while (a < path.size()) {
        size_t b = path.find(L';', a);
        if (b == std::wstring::npos) b = path.size();
        std::wstring d = path.substr(a, b - a);
        if (!d.empty() && fileExists(withSlash(d) + L"cmake.exe")) {
            // child processes get the same PATH (git, clang, msbuild...)
            SetEnvironmentVariableW(L"PATH", path.c_str());
            return withSlash(d) + L"cmake.exe";
        }
        a = b + 1;
    }
    return {};
}

static void refreshState() {
    std::wstring bin = binDir();
    for (auto& c : g_comps) {
        c.state = 0;
        if (app.gdDir.empty()) continue;
        std::wstring inGame = withSlash(app.gdDir) + c.dll;
        if (!fileExists(inGame)) continue;
        if (!fileExists(bin + c.dll)) c.state = 3;
        else c.state = sameFile(inGame, bin + c.dll) ? 1 : 2;
    }
}

static void saveSettings() {
    WritePrivateProfileStringW(L"Installer", L"gd_folder", app.gdDir.c_str(), app.settingsIni.c_str());
    WritePrivateProfileStringW(L"Installer", L"src_folder", app.srcDir.c_str(), app.settingsIni.c_str());
    WritePrivateProfileStringW(L"Installer", L"compile", app.compile ? L"1" : L"0", app.settingsIni.c_str());
    WritePrivateProfileStringW(L"Installer", L"clean", app.cleanBuild ? L"1" : L"0", app.settingsIni.c_str());
    WritePrivateProfileStringW(L"Installer", L"shortcuts", app.shortcuts ? L"1" : L"0", app.settingsIni.c_str());
    WritePrivateProfileStringW(L"Installer", L"apps", app.installApps ? L"1" : L"0", app.settingsIni.c_str());
    for (int i = 0; i < kComps; i++)
        WritePrivateProfileStringW(L"Installer", g_comps[i].dll, g_comps[i].want ? L"1" : L"0", app.settingsIni.c_str());
}

// ---------------------------------------------------------------------------
// Steps (run on a worker thread)
// ---------------------------------------------------------------------------
static bool runCommand(const std::wstring& cmdLine, const std::wstring& cwd) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdLine;
    BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        logLine(4, "Could not start: %s", narrow(cmdLine).c_str());
        return false;
    }
    // stream the output into the log, line by line
    std::string pending;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            int kind = 5;
            if (line.find(": error") != std::string::npos || line.find("error MSB") != std::string::npos ||
                line.find("CMake Error") != std::string::npos)
                kind = 4;
            else if (line.find("warning") != std::string::npos) kind = 3;
            logLine(kind, "  %s", line.c_str());
        }
    }
    if (!pending.empty()) logLine(5, "  %s", pending.c_str());
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return code == 0;
}

static bool stepCloseApps() {
    setStep("Closing Geometry Dash and the Kuang apps...", 5);
    bool ok = true;
    for (auto e : kWatched) {
        if (findProcesses(e).empty()) continue;
        bool gd = _wcsicmp(e, L"GeometryDash.exe") == 0;
        logLine(0, "Closing %s%s", narrow(e).c_str(), gd ? " (it saves your progress first)" : "");
        if (!closeProcess(e, gd ? 12000 : 4000)) {
            logLine(4, "Could not close %s. Close it yourself and try again.", narrow(e).c_str());
            ok = false;
        }
    }
    if (ok) logLine(2, "Nothing is using the files anymore.");
    return ok;
}

// deletes a folder tree; files that are in use are skipped (counted) instead of stopping everything
static int deleteTree(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    int failed = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (it->is_directory(ec) && !it->is_symlink(ec)) failed += deleteTree(p);
        else {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); // read-only files too
            if (!fs::remove(p, ec)) failed++;
        }
    }
    fs::remove(dir, ec);
    return failed;
}

// clean build: nothing from an older compile can end up installed (build\ and dist\ are made again)
static bool stepClean() {
    if (app.srcDir.empty()) return true;
    setStep("Clean build: deleting build\\ and dist\\...", 10);
    int failed = 0;
    for (const wchar_t* d : {L"build", L"dist"}) {
        std::wstring path = app.srcDir + d;
        if (!dirExists(path)) continue;
        int f = deleteTree(path);
        failed += f;
        if (f == 0) logLine(2, "%s\\ deleted", narrow(d).c_str());
        else logLine(3, "%s\\: %d file(s) in use could not be deleted (they will be overwritten)", narrow(d).c_str(), f);
    }
    return true; // a few locked files are overwritten by the build anyway
}

static bool stepCompile() {
    if (app.srcDir.empty()) { logLine(4, "Source folder not found (the one with CMakeLists.txt)."); return false; }
    std::wstring cmake = app.cmakePath;
    if (cmake.empty()) {
        logLine(4, "CMake was not found. Run instalar-herramientas.bat first, or turn off \"Compile\".");
        return false;
    }
    std::wstring q = L"\"" + cmake + L"\"";
    std::wstring src = app.srcDir.substr(0, app.srcDir.size() - 1);
    setStep("Preparing the build (CMake)...", 15);
    if (!runCommand(q + L" -S \"" + src + L"\" -B \"" + src + L"\\build\" -A x64 -T ClangCL", src)) {
        logLine(4, "CMake could not prepare the build. Are the Visual Studio Build Tools (with Clang) installed?");
        return false;
    }
    setStep("Compiling (this takes a minute the first time)...", 30);
    // the installer itself is not rebuilt here: it is running.
    // One target per call, run inside the build folder: with several --target values the Visual Studio
    // generator looked for "kfe.vcxproj" relative to the wrong folder (MSB1009).
    static const wchar_t* kTargets[] = {L"kfe", L"winmm_proxy", L"kopt", L"kmenu", L"app", L"optimizer_app"};
    std::wstring bdir = src + L"\\build";
    int n = 0;
    for (auto t : kTargets) {
        setStep((std::string("Compiling ") + narrow(t) + "...").c_str(), 30 + n * 7);
        n++;
        if (!runCommand(q + L" --build \"" + bdir + L"\" --config Release --target " + t, bdir)) {
            logLine(4, "Compiling failed. The red lines above say why.");
            return false;
        }
    }
    logLine(2, "Compiled.");
    return true;
}

static bool copyChecked(const std::wstring& from, const std::wstring& to) {
    for (int attempt = 0; attempt < 5; attempt++) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE) && sameFile(from, to)) return true;
        Sleep(400);
    }
    return false;
}

static bool stepInstallFiles() {
    std::wstring bin = binDir(), dst = withSlash(app.gdDir);
    setStep("Installing into the game folder...", 75);
    logLine(0, "From: %s", narrow(bin).c_str());
    logLine(0, "To:   %s", narrow(dst).c_str());
    bool anyWanted = false, ok = true;
    for (auto& c : g_comps) {
        if (!c.want) continue;
        anyWanted = true;
        if (!fileExists(bin + c.dll)) {
            logLine(4, "%s is missing in %s (compile first).", narrow(c.dll).c_str(), narrow(bin).c_str());
            ok = false;
        }
    }
    if (anyWanted && !fileExists(bin + KFE_PROXY_NAME)) { logLine(4, "winmm.dll is missing (compile first)."); ok = false; }
    if (!ok) return false;

    for (auto& c : g_comps) {
        std::wstring target = dst + c.dll;
        if (c.want) {
            if (copyChecked(bin + c.dll, target)) logLine(2, "%s  installed (%s)", c.name, narrow(c.dll).c_str());
            else { logLine(4, "%s could not be written. Is the game still open?", narrow(c.dll).c_str()); ok = false; }
        } else if (fileExists(target)) {
            if (DeleteFileW(target.c_str())) logLine(0, "%s  removed (unchecked)", c.name);
            else { logLine(4, "Could not remove %s.", narrow(c.dll).c_str()); ok = false; }
        }
    }
    // the shared loader
    std::wstring proxy = dst + KFE_PROXY_NAME;
    bool proxyOurs = fileExists(proxy) && fileContains(proxy, KFE_PROXY_MARKER);
    if (anyWanted) {
        if (fileExists(proxy) && !proxyOurs) {
            MoveFileExW(proxy.c_str(), (dst + L"winmm.dll.kfe-backup").c_str(), MOVEFILE_REPLACE_EXISTING);
            logLine(3, "Another winmm.dll was in the game folder: kept as winmm.dll.kfe-backup.");
        }
        if (copyChecked(bin + KFE_PROXY_NAME, proxy)) logLine(2, "Loader  installed (winmm.dll)");
        else { logLine(4, "winmm.dll could not be written."); ok = false; }
    } else if (proxyOurs) {
        DeleteFileW(proxy.c_str());
        if (fileExists(dst + L"winmm.dll.kfe-backup")) MoveFileExW((dst + L"winmm.dll.kfe-backup").c_str(), proxy.c_str(), 0);
        logLine(0, "Loader removed (no Kuang mod selected).");
    }
    // the old Geode version of the frame extrapolation would smooth twice
    std::wstring geodeMod = dst + L"geode\\mods\\kuang.frame-extrapolation.geode";
    if (g_comps[0].want && fileExists(geodeMod)) {
        MoveFileExW(geodeMod.c_str(), (geodeMod + L".disabled").c_str(), MOVEFILE_REPLACE_EXISTING);
        logLine(3, "Disabled the old Geode version of Frame Extrapolation.");
    }
    return ok;
}

// keep dist\ (where the apps live) up to date, so their own Install buttons copy the new files too
static void stepRefreshDist() {
    if (app.srcDir.empty() || !(app.compile || app.fromBuild)) return;
    std::wstring bin = binDir(), dist = distDir();
    if (bin == dist) return;
    setStep("Updating the apps in dist\\...", 88);
    CreateDirectoryW(dist.c_str(), nullptr);
    const wchar_t* files[] = {L"kfe.dll", L"kopt.dll", L"kmenu.dll", L"winmm.dll", L"KuangFrameExtrapolation.exe", L"KuangOptimizer.exe"};
    for (auto f : files) {
        if (!fileExists(bin + f)) continue;
        if (!copyChecked(bin + f, dist + f)) logLine(3, "Could not update dist\\%s (is it open?)", narrow(f).c_str());
    }
    logLine(2, "Apps ready in %s", narrow(dist).c_str());
}

// the control programs, with the DLLs next to them (their own Install buttons copy from there)
static void stepInstallApps() {
    if (!app.installApps) return;
    std::wstring bin = binDir(), dir = appsDir();
    bool any = false;
    for (auto& c : g_comps) any |= c.want && c.app && fileExists(bin + c.app);
    if (!any) return;
    setStep("Installing the control programs...", 90);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const wchar_t* dlls[] = {L"kfe.dll", L"kopt.dll", L"kmenu.dll", L"winmm.dll"};
    for (auto d : dlls) if (fileExists(bin + d)) copyChecked(bin + d, dir + d);
    for (auto& c : g_comps) {
        if (!c.app || !fileExists(bin + c.app)) continue;
        if (c.want) {
            if (copyChecked(bin + c.app, dir + c.app)) logLine(2, "%s app  ready", c.name);
            else logLine(3, "Could not copy %s (is it open?)", narrow(c.app).c_str());
        } else if (fileExists(dir + c.app)) {
            DeleteFileW((dir + c.app).c_str());
        }
    }
    logLine(0, "Programs folder: %s", narrow(dir).c_str());
}

static void makeShortcut(const std::wstring& target, const std::wstring& name) {
    wchar_t desk[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) return;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save((std::wstring(desk) + L"\\" + name + L".lnk").c_str(), TRUE);
        pf->Release();
    }
    link->Release();
}

static void stepShortcuts() {
    if (!app.shortcuts) return;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring dist = app.installApps ? appsDir() : (app.srcDir.empty() ? exeDir() : distDir());
    for (auto& c : g_comps) {
        if (!c.want || !c.app || !fileExists(dist + c.app)) continue;
        makeShortcut(dist + c.app, widen(c.name));
        logLine(2, "Desktop shortcut: %s", c.name);
    }
    CoUninitialize();
}

static void installWorker() {
    app.result = 0;
    bool ok = stepCloseApps();
    if (ok && app.compile && app.cleanBuild) stepClean();
    if (ok && app.compile) ok = stepCompile();
    if (ok) ok = stepInstallFiles();
    if (ok) { stepRefreshDist(); stepInstallApps(); stepShortcuts(); }
    refreshState();
    app.progress = 100;
    if (ok) {
        setStep("Done! Open Geometry Dash and press Tab for the menu.", 100);
        logLine(2, "Everything is installed and checked.");
    } else {
        setStep("Something failed - read the red lines.", 100);
    }
    app.result = ok ? 1 : 2;
    app.busy = false;
}

static void uninstallWorker() {
    app.result = 0;
    bool ok = stepCloseApps();
    if (ok) {
        setStep("Removing the mods from the game folder...", 50);
        std::wstring dst = withSlash(app.gdDir);
        for (auto& c : g_comps) {
            if (!fileExists(dst + c.dll)) continue;
            if (DeleteFileW((dst + c.dll).c_str())) logLine(2, "%s removed", c.name);
            else { logLine(4, "Could not remove %s", narrow(c.dll).c_str()); ok = false; }
        }
        std::wstring proxy = dst + KFE_PROXY_NAME;
        if (fileExists(proxy) && fileContains(proxy, KFE_PROXY_MARKER)) {
            if (DeleteFileW(proxy.c_str())) logLine(2, "Loader removed");
            else { logLine(4, "Could not remove winmm.dll"); ok = false; }
            if (fileExists(dst + L"winmm.dll.kfe-backup")) MoveFileExW((dst + L"winmm.dll.kfe-backup").c_str(), proxy.c_str(), 0);
        }
        std::wstring dir = appsDir();
        if (dirExists(dir)) {
            const wchar_t* files[] = {L"kfe.dll", L"kopt.dll", L"kmenu.dll", L"winmm.dll", L"KuangFrameExtrapolation.exe", L"KuangOptimizer.exe"};
            for (auto f : files) DeleteFileW((dir + f).c_str());
            RemoveDirectoryW(dir.c_str());
            wchar_t desk[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk)))
                for (auto& c : g_comps) DeleteFileW((std::wstring(desk) + L"\\" + widen(c.name) + L".lnk").c_str());
            logLine(2, "Control programs removed");
        }
        logLine(0, "Your settings (kfe.ini, kopt.ini, kmenu.ini) were kept.");
    }
    refreshState();
    setStep(ok ? "Uninstalled. Geometry Dash is back to normal." : "Some files could not be removed.", 100);
    app.result = ok ? 1 : 2;
    app.busy = false;
}

static void startAction(int action) {
    if (app.busy) return;
    if (app.gdDir.empty()) { logLine(4, "Select your Geometry Dash folder first."); return; }
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        app.log.clear();
    }
    saveSettings();
    app.busy = true;
    app.progress = 0;
    std::thread(action == 1 ? installWorker : uninstallWorker).detach();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static ImFont* g_fontBody = nullptr;
static ImFont* g_fontBold = nullptr;
static ImFont* g_fontTitle = nullptr;
static ImFont* g_fontMono = nullptr;
static float g_scale = 1.0f;

namespace col {
    const ImVec4 bg      {0.204f, 0.208f, 0.216f, 1};
    const ImVec4 card    {0.247f, 0.251f, 0.263f, 1};
    const ImVec4 card2   {0.290f, 0.294f, 0.306f, 1};
    const ImVec4 text    {0.93f, 0.93f, 0.94f, 1};
    const ImVec4 dim     {0.66f, 0.67f, 0.70f, 1};
    const ImVec4 accent  {0.40f, 0.70f, 1.00f, 1};
    const ImVec4 green   {0.33f, 0.80f, 0.52f, 1};
    const ImVec4 yellow  {0.98f, 0.78f, 0.35f, 1};
    const ImVec4 red     {1.00f, 0.45f, 0.42f, 1};
}
static ImU32 u32(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
static float S(float v) { return v * g_scale; }

static void beginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
static void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(6)));
}
static void sectionTitle(const char* t) {
    ImGui::PushFont(g_fontBold);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(2)));
}
static void dimText(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
static bool button(const char* label, const ImVec4& c, float w = 0, bool enabled = true, float h = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.f, c.x * 1.12f), std::min(1.f, c.y * 1.12f), std::min(1.f, c.z * 1.12f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.9f, c.y * 0.9f, c.z * 0.9f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(8)));
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, ImVec2(w, h));
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    return r;
}

// opens a mod's settings: the KFE / Optimizer app, or the menu's settings file
static void openSettings(const Component& c) {
    if (c.app) {
        std::wstring candidates[] = {appsDir() + c.app, app.srcDir.empty() ? std::wstring() : distDir() + c.app, exeDir() + c.app};
        for (auto& path : candidates) {
            if (path.empty() || !fileExists(path)) continue;
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, path.substr(0, path.find_last_of(L'\\')).c_str(), SW_SHOWNORMAL);
            logLine(2, "Opened %s", narrow(c.app).c_str());
            return;
        }
        logLine(4, "%s is not installed yet: check \"Install the control programs\" and press Install / Update.", narrow(c.app).c_str());
        return;
    }
    // Kuang Menu lives inside the game
    logLine(1, "Kuang Menu: its settings are inside the game - press Tab while playing.");
    if (!app.gdDir.empty()) {
        std::wstring ini = withSlash(app.gdDir) + L"kmenu.ini";
        if (fileExists(ini)) {
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + ini + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            logLine(0, "Opened kmenu.ini (the file where the menu saves everything).");
        }
    }
}

// a mod row: check box, name, description and a status chip
static bool compRow(Component& c) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(62);
    ImGui::PushID(c.name);
    ImGui::SetNextItemAllowOverlap(); // the Settings button sits on top of the row
    bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (pressed && !app.busy) c.want = !c.want;
    auto dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(hovered ? col::card2 : ImVec4(0.27f, 0.275f, 0.29f, 1)), S(10));
    if (c.want) dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(col::accent, 0.8f), S(10), 0, S(1.5f));
    // check box
    ImVec2 cb(p.x + S(14), p.y + (h - S(20)) * 0.5f);
    dl->AddRectFilled(cb, ImVec2(cb.x + S(20), cb.y + S(20)), u32(c.want ? col::accent : ImVec4(0.2f, 0.2f, 0.21f, 1)), S(5));
    if (c.want) {
        dl->AddLine(ImVec2(cb.x + S(4.5f), cb.y + S(10.5f)), ImVec2(cb.x + S(8.5f), cb.y + S(14.5f)), u32(ImVec4(1, 1, 1, 1)), S(2.2f));
        dl->AddLine(ImVec2(cb.x + S(8.5f), cb.y + S(14.5f)), ImVec2(cb.x + S(15.5f), cb.y + S(6)), u32(ImVec4(1, 1, 1, 1)), S(2.2f));
    }
    // text
    ImGui::PushFont(g_fontBold);
    dl->AddText(ImVec2(p.x + S(46), p.y + S(10)), u32(col::text), c.name);
    ImGui::PopFont();
    dl->AddText(ImVec2(p.x + S(46), p.y + S(34)), u32(col::dim), c.desc);
    // status chip
    const char* st = "Not installed";
    ImVec4 sc = col::dim;
    if (c.state == 1) { st = "Up to date"; sc = col::green; }
    if (c.state == 2) { st = "Update ready"; sc = col::yellow; }
    if (c.state == 3) { st = "Installed"; sc = col::accent; }
    ImVec2 ts = ImGui::CalcTextSize(st);
    ImVec2 cp(p.x + w - ts.x - S(28), p.y + S(10));
    dl->AddRectFilled(cp, ImVec2(cp.x + ts.x + S(16), cp.y + ts.y + S(6)), u32(sc, 0.18f), S(20));
    dl->AddText(ImVec2(cp.x + S(8), cp.y + S(3)), u32(sc), st);
    // "Settings" opens the mod's own settings (its app, or the in-game menu's file)
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - S(108), p.y + h - S(28)));
    ImGui::PushID(c.name);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.205f, 0.215f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.33f, 0.35f, 1));
    if (ImGui::Button("Settings", ImVec2(S(94), 0))) openSettings(c);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(after);
    return pressed;
}

static void drawUI(HWND hwnd) {
    double now = ImGui::GetTime();
    if (now - app.lastPoll > 0.7) {
        app.lastPoll = now;
        app.running = runningApps();
        if (!app.busy) refreshState();
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("Kuang Installer");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::Text("Compiles and installs every Kuang mod for Geometry Dash 2.2091  -  v%s", INSTALLER_VERSION);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(6)));

    // ---- mods
    beginCard("mods");
    sectionTitle("Mods");
    ImGui::BeginDisabled(app.busy);
    for (auto& c : g_comps) {
        if (compRow(c)) saveSettings();
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    ImGui::EndDisabled();
    dimText("Unchecked mods are removed from the game when you install. All of them share one small loader (winmm.dll).");
    endCard();

    // ---- folders and options
    beginCard("folders");
    sectionTitle("Folders");
    auto folderLine = [&](const char* label, const std::wstring& path, const char* id, bool gd) {
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        std::string s = path.empty() ? "(not found - click Change)" : narrow(path);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - S(110));
        ImGui::TextColored(path.empty() ? col::red : col::text, "%s", s.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - S(90));
        ImGui::PushID(id);
        if (button("Change", col::card2, S(90), !app.busy)) {
            std::wstring d = pickFolder(hwnd, gd ? L"Select the Geometry Dash folder (where GeometryDash.exe is)"
                                                 : L"Select the source folder (kfe-src, where CMakeLists.txt is)");
            if (!d.empty()) {
                if (gd) {
                    if (isGdFolder(d)) app.gdDir = d;
                    else logLine(4, "GeometryDash.exe is not in that folder.");
                } else {
                    if (fileExists(withSlash(d) + L"CMakeLists.txt")) app.srcDir = withSlash(d);
                    else logLine(4, "CMakeLists.txt is not in that folder.");
                }
                saveSettings();
                refreshState();
            }
        }
        ImGui::PopID();
    };
    folderLine("Geometry Dash", app.gdDir, "gd", true);
    ImGui::Dummy(ImVec2(0, S(2)));
    folderLine("Source code (kfe-src)", app.srcDir.empty() ? std::wstring() : app.srcDir, "src", false);
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::BeginDisabled(app.busy);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, col::accent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::card2);
    ImGui::BeginDisabled(app.srcDir.empty() || app.cmakePath.empty());
    if (ImGui::Checkbox("Compile from the source code first", &app.compile)) { saveSettings(); refreshState(); }
    ImGui::EndDisabled();
    ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    if (!app.payloadDir.empty())
        dimText(app.compile ? "Builds your latest code with CMake and the Visual Studio Build Tools, then installs it."
                            : "Off: installs the mods packed inside this installer (no build tools needed).");
    else if (app.cmakePath.empty())
        ImGui::TextColored(col::yellow, "CMake not found: run instalar-herramientas.bat once, or install the files already built.");
    else
        dimText("Uses CMake and the Visual Studio Build Tools, like compilar.bat. Turn it off to install the files already built.");
    ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(!app.compile);
    if (ImGui::Checkbox("Clean build (delete build\\ and dist\\ first)", &app.cleanBuild)) saveSettings();
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Install the control programs (KFE and Optimizer apps)", &app.installApps)) saveSettings();
    if (ImGui::Checkbox("Desktop shortcuts for the apps", &app.shortcuts)) saveSettings();
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    endCard();

    // ---- open programs warning
    if (!app.running.empty() && !app.busy) {
        std::string list;
        for (auto& r : app.running) list += (list.empty() ? "" : ", ") + narrow(r);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.36f, 0.30f, 0.16f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
        ImGui::BeginChild("open", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(col::yellow, "Open right now: %s", list.c_str());
        ImGui::TextUnformatted("They will be closed when you install (Geometry Dash is asked to close normally, so it saves).");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(6)));
    }

    // ---- actions
    {
        float w = ImGui::GetContentRegionAvail().x;
        bool anyWanted = false;
        for (auto& c : g_comps) anyWanted |= c.want;
        bool can = !app.busy && !app.gdDir.empty() && anyWanted;
        ImGui::PushFont(g_fontBold);
        if (button(app.busy ? "Working..." : "Install / Update", ImVec4(0.24f, 0.52f, 0.86f, 1), w * 0.68f, can, S(46))) {
            if (!app.running.empty()) { app.confirmClose = true; app.pendingAction = 1; ImGui::OpenPopup("Close programs?"); }
            else startAction(1);
        }
        ImGui::SameLine();
        if (button("Uninstall all", ImVec4(0.55f, 0.27f, 0.27f, 1), 0, !app.busy && !app.gdDir.empty(), S(46))) {
            if (!app.running.empty()) { app.confirmClose = true; app.pendingAction = 2; ImGui::OpenPopup("Close programs?"); }
            else startAction(2);
        }
        ImGui::PopFont();
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(16)));
    if (ImGui::BeginPopupModal("Close programs?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("These programs are open and will be closed:");
        for (auto& r : app.running) ImGui::BulletText("%s", narrow(r).c_str());
        ImGui::Dummy(ImVec2(0, S(4)));
        if (button("Close them and continue", ImVec4(0.24f, 0.52f, 0.86f, 1))) { startAction(app.pendingAction); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (button("Cancel", col::card2)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, S(6)));

    // ---- progress
    std::string step;
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        step = app.stepName;
    }
    if (!step.empty()) {
        float w = ImGui::GetContentRegionAvail().x, h = S(10);
        ImVec2 p = ImGui::GetCursorScreenPos();
        auto dl = ImGui::GetWindowDrawList();
        float t = app.progress / 100.0f;
        ImVec4 barCol = app.result == 2 ? col::red : app.result == 1 ? col::green : col::accent;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(col::card2), S(5));
        dl->AddRectFilled(p, ImVec2(p.x + w * t, p.y + h), u32(barCol), S(5));
        if (app.busy) {
            // moving shine while working
            float x = p.x + std::fmod(static_cast<float>(now) * 0.6f, 1.0f) * w * t;
            dl->AddRectFilled(ImVec2(std::max(p.x, x - S(30)), p.y), ImVec2(std::min(p.x + w * t, x + S(30)), p.y + h), u32(ImVec4(1, 1, 1, 0.25f)), S(5));
        }
        ImGui::Dummy(ImVec2(w, h + S(4)));
        ImGui::TextColored(barCol, "%s", step.c_str());
    }

    // ---- log
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.153f, 0.16f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild("log", ImVec2(0, std::max(S(120), ImGui::GetContentRegionAvail().y - S(26))), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        if (app.log.empty()) ImGui::TextColored(col::dim, "Choose the mods and press Install / Update. Progress shows here.");
        ImGui::PushFont(g_fontMono);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(app.log.size()));
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const LogLine& l = app.log[i];
                ImVec4 c = l.kind == 1 ? col::accent : l.kind == 2 ? col::green : l.kind == 3 ? col::yellow : l.kind == 4 ? col::red
                         : l.kind == 5 ? ImVec4(0.55f, 0.56f, 0.6f, 1) : col::text;
                ImGui::PushStyleColor(ImGuiCol_Text, c);
                ImGui::TextUnformatted(l.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopFont();
        if (app.scrollLog) { ImGui::SetScrollHereY(1.0f); app.scrollLog = false; }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col::dim.x, col::dim.y, col::dim.z, 0.7f));
    ImGui::TextUnformatted("Your .ini settings are never touched.  Made by Kuang.");
    ImGui::PopStyleColor();
    ImGui::End();
}

static void setupStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.PopupRounding = 10;
    s.ScrollbarRounding = 8;
    s.ScrollbarSize = 12;
    s.ItemSpacing = ImVec2(10, 7);
    s.FramePadding = ImVec2(8, 5);
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = col::bg;
    c[ImGuiCol_ChildBg] = col::card;
    c[ImGuiCol_PopupBg] = col::card;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_Text] = col::text;
    c[ImGuiCol_TextDisabled] = col::dim;
    c[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TitleBgActive] = col::card2;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.38f, 0.385f, 0.40f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.455f, 0.47f, 1);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.5f, 0.5f, 0.52f, 1);
    c[ImGuiCol_Button] = col::card2;
}

static void loadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::string fonts = narrow(windir) + "\\Fonts\\";
    auto load = [&](const char* file, float size) -> ImFont* {
        std::string p = fonts + file;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(p.c_str(), size * scale, &cfg);
    };
    g_fontBody = load("segoeui.ttf", 17);
    if (!g_fontBody) { ImFontConfig cfg; cfg.SizePixels = 15 * scale; g_fontBody = io.Fonts->AddFontDefault(&cfg); }
    g_fontBold = load("seguisb.ttf", 18);
    if (!g_fontBold) g_fontBold = g_fontBody;
    g_fontTitle = load("seguisb.ttf", 28);
    if (!g_fontTitle) g_fontTitle = g_fontBold;
    g_fontMono = load("consola.ttf", 14);
    if (!g_fontMono) g_fontMono = g_fontBody;
    io.FontDefault = g_fontBody;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    std::wstring args = cmdLine ? cmdLine : L"";
    app.autoMode = args.find(L"--auto") != std::wstring::npos;
    app.fromBuild = args.find(L"--from-build") != std::wstring::npos;
    // closing the Optimizer (which runs as administrator) and writing in Program Files need admin rights
    if (!isElevated()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", self, args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL)) > 32) return 0;
        // if the user says no, keep going without admin: most things still work
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"KuangInstallerSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"KuangInstaller", nullptr)) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        std::wstring d = std::wstring(appdata) + L"\\KuangFrameExtrapolation";
        CreateDirectoryW(d.c_str(), nullptr);
        app.settingsIni = d + L"\\installer.ini";
    }
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Installer", L"gd_folder", L"", buf, MAX_PATH, app.settingsIni.c_str());
    app.gdDir = (buf[0] && isGdFolder(buf)) ? std::wstring(buf) : findGdFolder();
    GetPrivateProfileStringW(L"Installer", L"src_folder", L"", buf, MAX_PATH, app.settingsIni.c_str());
    app.srcDir = (buf[0] && fileExists(withSlash(buf) + L"CMakeLists.txt")) ? withSlash(buf) : findSourceFolder();
    app.payloadDir = extractPayload();
    app.compile = GetPrivateProfileIntW(L"Installer", L"compile", app.payloadDir.empty() ? 1 : 0, app.settingsIni.c_str()) != 0;
    app.installApps = GetPrivateProfileIntW(L"Installer", L"apps", 1, app.settingsIni.c_str()) != 0;
    app.shortcuts = GetPrivateProfileIntW(L"Installer", L"shortcuts", 0, app.settingsIni.c_str()) != 0;
    app.cleanBuild = GetPrivateProfileIntW(L"Installer", L"clean", 1, app.settingsIni.c_str()) != 0;
    for (auto& c : g_comps) c.want = GetPrivateProfileIntW(L"Installer", c.dll, 1, app.settingsIni.c_str()) != 0;
    app.cmakePath = findCmake();
    if (app.cmakePath.empty() || app.srcDir.empty()) app.compile = false;
    if (app.fromBuild) app.compile = false; // already compiled by ACTUALIZAR.bat (not saved: it is per run)
    refreshState();

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc, 0, 0, inst, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"KuangInstaller", nullptr};
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Kuang Installer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(640, dpi, 96), MulDiv(900, dpi, 96), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF caption = RGB(52, 53, 55);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    if (!createDevice(hwnd)) {
        cleanupDevice();
        MessageBoxW(nullptr, L"Could not start DirectX 11.", L"Kuang Installer", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    g_scale = GetDpiForWindow(hwnd) / 96.0f;
    setupStyle(g_scale);
    loadFonts(g_scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    if (app.gdDir.empty()) logLine(3, "Geometry Dash was not found automatically: click Change and pick its folder.");
    if (!app.payloadDir.empty()) logLine(0, "This installer carries the mods inside: it works on its own, no source code needed.");
    else if (app.srcDir.empty()) logLine(3, "Source folder not found: compiling is off (the files already built will be installed).");
    if (app.autoMode) {
        bool anyWanted = false;
        for (auto& c : g_comps) anyWanted |= c.want;
        if (!anyWanted) for (auto& c : g_comps) c.want = true; // automatic update: everything
        if (app.gdDir.empty()) { logLine(4, "Automatic update: Geometry Dash was not found. Click Change, pick its folder and press Install."); app.autoMode = false; }
        else if (app.fromBuild && !fileExists(buildDir() + L"kmenu.dll")) { logLine(4, "Automatic update: build\\Release is empty (the compile failed?)."); app.autoMode = false; }
        else startAction(1); // the install closes Geometry Dash normally first, so it saves
    }

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        // automatic update: when everything worked, close by itself a few seconds later (errors stay on screen)
        if (app.autoMode && !app.busy && app.result == 1) {
            if (!app.autoDoneAt) app.autoDoneAt = GetTickCount64();
            else if (GetTickCount64() - app.autoDoneAt > 4000) { PostMessageW(hwnd, WM_CLOSE, 0, 0); app.autoMode = false; }
        }
        if (IsIconic(hwnd)) { Sleep(50); continue; }
        if (g_resizeW && g_resizeH) {
            cleanupRT();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            createRT();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        drawUI(hwnd);
        ImGui::Render();
        const float clear[4] = {col::bg.x, col::bg.y, col::bg.z, 1};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }
    // don't leave a half-done install behind: wait for the worker
    while (app.busy) Sleep(100);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
