// Kuang Frame Extrapolation - control app.
// Shows what the mod does, turns it on/off (live, through kfe.ini), changes its
// options and installs/uninstalls it in the Geometry Dash folder.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "../shared/kfe_shared.h"

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example)
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
            mm->ptMinTrackSize.x = MulDiv(520, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(500, dpi, 96);
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
// Helpers
// ---------------------------------------------------------------------------
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
static std::wstring withSlash(std::wstring s) {
    if (!s.empty() && s.back() != L'\\' && s.back() != L'/') s += L'\\';
    return s;
}
static bool isGdFolder(const std::wstring& dir) { return fileExists(withSlash(dir) + L"GeometryDash.exe"); }

static std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
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
        FILE* f = _wfopen((withSlash(steam) + L"steamapps\\libraryfolders.vdf").c_str(), L"rb");
        if (f) {
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
            size_t pos = 0;
            while ((pos = text.find("\"path\"", pos)) != std::string::npos) {
                size_t a = text.find('"', pos + 6);
                size_t b = a == std::string::npos ? a : text.find('"', a + 1);
                if (b == std::string::npos) break;
                std::string p = text.substr(a + 1, b - a - 1);
                std::string clean;
                for (size_t i = 0; i < p.size(); i++) {
                    if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++;
                    clean += p[i];
                }
                int wn = MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), -1, nullptr, 0);
                std::wstring w(wn > 0 ? wn - 1 : 0, L'\0');
                if (wn > 1) MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), -1, w.data(), wn);
                libs.push_back(w);
                pos = b;
            }
        }
    }
    for (auto& l : libs) {
        std::wstring d = withSlash(l) + L"steamapps\\common\\Geometry Dash";
        if (isGdFolder(d)) return d;
    }
    // the app placed inside the GD folder
    std::wstring here = exeDir();
    if (isGdFolder(here)) return here.substr(0, here.size() - 1);
    return {};
}

static std::wstring pickFolder(HWND owner) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Select your Geometry Dash folder (where GeometryDash.exe is)");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

static bool gdRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"GeometryDash.exe") == 0) { found = true; break; }
    }
    CloseHandle(snap);
    return found;
}

static bool fileContains(const std::wstring& path, const char* marker) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    return data.find(marker) != std::string::npos;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
struct Options {
    bool enabled = true;
    int mode = 0; // 0 extrapolation, 1 interpolation
    bool smart = true;
    bool background = true;
    bool rotation = true;
    bool objects = true;
    bool trails = true;
    bool debug = false;
};

struct App {
    std::wstring gdDir;
    std::wstring appIni;
    Options opt;
    bool installed = false;
    bool proxyIsOurs = false;
    bool otherWinmm = false;
    bool geodeVersionPresent = false;
    bool running = false;
    KfeStatus status{};
    bool statusValid = false;
    std::string toast;
    ImVec4 toastColor{1, 1, 1, 1};
    double toastUntil = 0;
    double lastPoll = -10;
} app;

static std::wstring gdIni() { return withSlash(app.gdDir) + KFE_INI_NAME; }

static void showToast(const std::string& msg, bool error = false) {
    app.toast = msg;
    app.toastColor = error ? ImVec4(1.0f, 0.45f, 0.42f, 1) : ImVec4(0.45f, 0.86f, 0.6f, 1);
    app.toastUntil = ImGui::GetTime() + 5.0;
}

static void loadOptions() {
    if (app.gdDir.empty()) return;
    auto p = gdIni();
    if (!fileExists(p)) return;
    auto geti = [&](const wchar_t* k, int d) { return GetPrivateProfileIntW(L"General", k, d, p.c_str()) != 0; };
    app.opt.enabled = geti(L"enabled", 1);
    app.opt.smart = geti(L"smart_prediction", 1);
    app.opt.background = geti(L"smooth_background", 1);
    app.opt.rotation = geti(L"smooth_rotation", 1);
    app.opt.objects = geti(L"smooth_objects", 1);
    app.opt.trails = geti(L"smooth_trails", 1);
    app.opt.debug = geti(L"debug_log", 0);
    wchar_t mode[64] = {};
    GetPrivateProfileStringW(L"General", L"mode", L"extrapolation", mode, 64, p.c_str());
    app.opt.mode = _wcsicmp(mode, L"interpolation") == 0 ? 1 : 0;
}

static void saveOptions() {
    if (app.gdDir.empty()) return;
    auto p = gdIni();
    auto put = [&](const wchar_t* k, bool v) { WritePrivateProfileStringW(L"General", k, v ? L"1" : L"0", p.c_str()); };
    put(L"enabled", app.opt.enabled);
    WritePrivateProfileStringW(L"General", L"mode", app.opt.mode ? L"interpolation" : L"extrapolation", p.c_str());
    put(L"smart_prediction", app.opt.smart);
    put(L"smooth_background", app.opt.background);
    put(L"smooth_rotation", app.opt.rotation);
    put(L"smooth_objects", app.opt.objects);
    put(L"smooth_trails", app.opt.trails);
    put(L"debug_log", app.opt.debug);
}

static void refreshInstallState() {
    app.installed = app.proxyIsOurs = app.otherWinmm = app.geodeVersionPresent = false;
    if (app.gdDir.empty()) return;
    std::wstring d = withSlash(app.gdDir);
    bool proxy = fileExists(d + KFE_PROXY_NAME);
    app.proxyIsOurs = proxy && fileContains(d + KFE_PROXY_NAME, KFE_PROXY_MARKER);
    app.otherWinmm = proxy && !app.proxyIsOurs;
    app.installed = app.proxyIsOurs && fileExists(d + KFE_DLL_NAME);
    app.geodeVersionPresent = fileExists(d + L"geode\\mods\\kuang.frame-extrapolation.geode");
}

static void setGdDir(const std::wstring& dir) {
    app.gdDir = dir;
    WritePrivateProfileStringW(L"App", L"gd_folder", dir.c_str(), app.appIni.c_str());
    refreshInstallState();
    loadOptions();
}

static void pollStatus() {
    double now = ImGui::GetTime();
    if (now - app.lastPoll < 0.25) return;
    app.lastPoll = now;
    app.running = gdRunning();
    app.statusValid = false;
    if (!app.running) return;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, KFE_STATUS_MAPPING);
    if (!h) return;
    if (auto v = static_cast<KfeStatus*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(KfeStatus)))) {
        app.status = *v;
        UnmapViewOfFile(v);
        app.statusValid = app.status.magic == KFE_STATUS_MAGIC;
    }
    CloseHandle(h);
}

static bool doInstall() {
    if (app.gdDir.empty()) { showToast("Select your Geometry Dash folder first.", true); return false; }
    if (gdRunning()) { showToast("Close Geometry Dash first, then install.", true); return false; }
    std::wstring src = exeDir(), dst = withSlash(app.gdDir);
    if (!fileExists(src + KFE_DLL_NAME) || !fileExists(src + KFE_PROXY_NAME)) {
        showToast("kfe.dll / winmm.dll are missing next to this app.", true);
        return false;
    }
    if (app.otherWinmm) {
        // keep whatever was there before
        MoveFileExW((dst + KFE_PROXY_NAME).c_str(), (dst + L"winmm.dll.kfe-backup").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (!CopyFileW((src + KFE_DLL_NAME).c_str(), (dst + KFE_DLL_NAME).c_str(), FALSE) ||
        !CopyFileW((src + KFE_PROXY_NAME).c_str(), (dst + KFE_PROXY_NAME).c_str(), FALSE)) {
        showToast("Could not copy the files (is the folder read-only?).", true);
        refreshInstallState();
        return false;
    }
    // the Geode version would smooth a second time
    std::wstring geodeMod = dst + L"geode\\mods\\kuang.frame-extrapolation.geode";
    if (fileExists(geodeMod))
        MoveFileExW(geodeMod.c_str(), (geodeMod + L".disabled").c_str(), MOVEFILE_REPLACE_EXISTING);
    saveOptions();
    refreshInstallState();
    showToast("Installed! Start Geometry Dash to use it.");
    return true;
}

static bool doUninstall() {
    if (gdRunning()) { showToast("Close Geometry Dash first, then uninstall.", true); return false; }
    std::wstring dst = withSlash(app.gdDir);
    bool ok = true;
    if (fileExists(dst + KFE_DLL_NAME)) ok &= DeleteFileW((dst + KFE_DLL_NAME).c_str()) != 0;
    // the loader is shared with Kuang Optimizer and Kuang Menu: keep it while one of them is installed
    bool otherMod = fileExists(dst + L"kopt.dll") || fileExists(dst + L"kmenu.dll");
    if (app.proxyIsOurs && !otherMod) {
        ok &= DeleteFileW((dst + KFE_PROXY_NAME).c_str()) != 0;
        if (fileExists(dst + L"winmm.dll.kfe-backup"))
            MoveFileExW((dst + L"winmm.dll.kfe-backup").c_str(), (dst + KFE_PROXY_NAME).c_str(), 0);
    }
    refreshInstallState();
    if (ok) showToast("Uninstalled. Your settings file (kfe.ini) was kept.");
    else showToast("Some files could not be removed.", true);
    return ok;
}

// ---------------------------------------------------------------------------
// UI pieces
// ---------------------------------------------------------------------------
static ImFont* g_fontBody = nullptr;
static ImFont* g_fontBold = nullptr;
static ImFont* g_fontTitle = nullptr;
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

static void beginCard(const char* id, float height = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild(id, ImVec2(0, height), height == 0 ? ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding
                                                        : ImGuiChildFlags_AlwaysUseWindowPadding);
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

static void dotText(const ImVec4& c, const char* label, const char* value) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + S(5), p.y + h * 0.5f), S(4.5f), u32(c));
    ImGui::Dummy(ImVec2(S(16), h));
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(value);
}

// a big on/off switch with an animated knob
static bool bigToggle(const char* id, bool* v) {
    float h = S(56), w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (pressed) *v = !*v;
    bool hovered = ImGui::IsItemHovered();
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetID(id);
    float t = st->GetFloat(key, *v ? 1.f : 0.f);
    t += ((*v ? 1.f : 0.f) - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    st->SetFloat(key, t);

    auto dl = ImGui::GetWindowDrawList();
    ImVec4 off = hovered ? col::card2 : ImVec4(0.27f, 0.275f, 0.29f, 1);
    ImVec4 on = col::green;
    ImVec4 c(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(c), S(12));
    // switch
    float sw = S(54), sh = S(28);
    ImVec2 sp(p.x + w - sw - S(16), p.y + (h - sh) * 0.5f);
    dl->AddRectFilled(sp, ImVec2(sp.x + sw, sp.y + sh), u32(ImVec4(0, 0, 0, 0.25f)), sh * 0.5f);
    float kx = sp.x + sh * 0.5f + (sw - sh) * t;
    dl->AddCircleFilled(ImVec2(kx, sp.y + sh * 0.5f), sh * 0.5f - S(3), u32(ImVec4(1, 1, 1, 1)));
    // label
    ImGui::PushFont(g_fontBold);
    const char* label = *v ? "ENABLED" : "DISABLED";
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + S(18), p.y + (h - ts.y) * 0.5f - S(8)), u32(col::text), label);
    ImGui::PopFont();
    const char* sub = *v ? "Click to turn the smoothing off" : "Click to turn the smoothing on";
    dl->AddText(ImVec2(p.x + S(18), p.y + h * 0.5f + S(3)), u32(col::text, 0.75f), sub);
    return pressed;
}

static bool modeCard(const char* id, const char* title, const char* desc, bool selected, float w) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(92);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    auto dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(hovered && !selected ? col::card2 : ImVec4(0.27f, 0.275f, 0.29f, 1)), S(10));
    if (selected) dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(col::accent), S(10), 0, S(2));
    // radio dot
    ImVec2 c(p.x + S(18), p.y + S(20));
    dl->AddCircle(c, S(7), u32(selected ? col::accent : col::dim), 0, S(1.6f));
    if (selected) dl->AddCircleFilled(c, S(3.5f), u32(col::accent));
    ImGui::PushFont(g_fontBold);
    dl->AddText(ImVec2(p.x + S(32), p.y + S(10)), u32(col::text), title);
    ImGui::PopFont();
    ImGui::PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddText(g_fontBody, g_fontBody->FontSize * 0.92f, ImVec2(p.x + S(14), p.y + S(38)), u32(col::dim), desc, nullptr, w - S(26));
    ImGui::PopClipRect();
    return pressed;
}

static bool optionRow(const char* label, const char* desc, bool* v) {
    ImGui::PushStyleColor(ImGuiCol_CheckMark, col::accent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::card2);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.34f, 0.345f, 0.36f, 1));
    bool changed = ImGui::Checkbox(label, v);
    ImGui::PopStyleColor(3);
    ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::Dummy(ImVec2(0, S(4)));
    return changed;
}

static bool button(const char* label, const ImVec4& c, float w = 0, bool enabled = true) {
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.f, c.x * 1.12f), std::min(1.f, c.y * 1.12f), std::min(1.f, c.z * 1.12f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.9f, c.y * 0.9f, c.z * 0.9f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(8)));
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, ImVec2(w, 0));
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    return r;
}

// Animated explanation: physics ticks vs. your frames
static void drawDiagram() {
    float w = ImGui::GetContentRegionAvail().x, h = S(150);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    auto dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(ImVec4(0.18f, 0.185f, 0.195f, 1)), S(8));

    float left = p.x + S(118), right = p.x + w - S(14);
    float span = right - left;
    double time = ImGui::GetTime();
    // 7 physics ticks across the width, frames at a non-matching rate
    const int ticks = 7;
    float tickW = span / ticks;
    float frameW = tickW * 0.62f;
    float scroll = static_cast<float>(std::fmod(time * 0.35, 1.0)) * frameW;

    auto label = [&](float y, const char* t, const ImVec4& c) {
        dl->AddText(ImVec2(p.x + S(12), y - ImGui::GetTextLineHeight() * 0.5f), u32(c), t);
    };
    float yTicks = p.y + S(22), yFrames = p.y + S(48), yWithout = p.y + S(88), yWith = p.y + S(124);
    label(yTicks, "Physics (240/s)", col::dim);
    label(yFrames, "Your frames", col::dim);
    label(yWithout, "Without mod", col::red);
    label(yWith, "With mod", col::green);

    for (int i = 0; i <= ticks; i++) {
        float x = left + i * tickW;
        dl->AddLine(ImVec2(x, yTicks - S(7)), ImVec2(x, yTicks + S(7)), u32(col::dim, 0.9f), S(2));
    }
    dl->AddLine(ImVec2(left, yTicks), ImVec2(right, yTicks), u32(col::dim, 0.35f), S(1));
    std::vector<float> frames;
    for (float x = left + frameW - scroll; x <= right; x += frameW) frames.push_back(x);
    for (float x : frames) dl->AddCircleFilled(ImVec2(x, yFrames), S(4), u32(col::accent));
    dl->AddLine(ImVec2(left, yFrames), ImVec2(right, yFrames), u32(col::accent, 0.25f), S(1));

    // position of the icon drawn at each frame
    float rise = S(18);
    auto trueY = [&](float x, float base) { return base + rise * 0.5f - rise * (x - left) / span; };
    auto snapX = [&](float x) { return left + std::floor((x - left) / tickW + 0.5f) * tickW; };
    // ideal motion (thin)
    dl->AddLine(ImVec2(left, trueY(left, yWithout)), ImVec2(right, trueY(right, yWithout)), u32(col::dim, 0.25f), S(1));
    dl->AddLine(ImVec2(left, trueY(left, yWith)), ImVec2(right, trueY(right, yWith)), u32(col::dim, 0.25f), S(1));
    for (size_t i = 0; i < frames.size(); i++) {
        float x = frames[i];
        float sx = snapX(x); // without: shows the nearest physics tick
        dl->AddRectFilled(ImVec2(x - S(5), trueY(sx, yWithout) - S(5)), ImVec2(x + S(5), trueY(sx, yWithout) + S(5)), u32(col::red), S(2));
        dl->AddRectFilled(ImVec2(x - S(5), trueY(x, yWith) - S(5)), ImVec2(x + S(5), trueY(x, yWith) + S(5)), u32(col::green), S(2));
    }
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
static void drawUI(HWND hwnd) {
    pollStatus();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    // ---- header
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("Kuang Frame Extrapolation");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::Text("Smooth camera and icons for Geometry Dash 2.2091  -  v%s", KFE_VERSION_STR);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(8)));

    // ---- status
    beginCard("status");
    {
        dotText(app.running ? col::green : col::dim, "Geometry Dash:", app.running ? "Running" : "Not running");
        const char* modText = "Not installed";
        ImVec4 modCol = col::dim;
        char buf[256];
        if (app.installed) { modText = "Installed - starts with the game"; modCol = col::accent; }
        if (app.running) {
            if (app.statusValid) {
                switch (app.status.state) {
                    case KFE_STATE_STARTING: modText = "Loaded - waiting for the main menu"; modCol = col::yellow; break;
                    case KFE_STATE_ACTIVE:
                        modText = app.status.enabled ? "Active" : "Loaded - smoothing is off";
                        modCol = app.status.enabled ? col::green : col::yellow;
                        break;
                    default:
                        std::snprintf(buf, sizeof(buf), "Error - %s", app.status.message);
                        modText = buf;
                        modCol = col::red;
                        break;
                }
            } else if (app.installed) {
                modText = "Not loaded - restart Geometry Dash";
                modCol = col::yellow;
            }
        }
        ImGui::PushTextWrapPos(0);
        dotText(modCol, "Mod:", modText);
        ImGui::PopTextWrapPos();
        if (app.running && app.statusValid && app.status.state == KFE_STATE_ACTIVE &&
            GetTickCount64() - app.status.heartbeat < 2000) {
            char live[160];
            if (app.status.inLevel && app.status.enabled)
                std::snprintf(live, sizeof(live), "%.0f FPS  -  camera/icons: %u  -  moving objects: %u", app.status.fps,
                    app.status.nodes, app.status.objects);
            else
                std::snprintf(live, sizeof(live), "%.0f FPS  -  enter a level to see it work", app.status.fps);
            dotText(col::accent, "Live:", live);
        }
    }
    endCard();

    // ---- big toggle
    if (bigToggle("##toggle", &app.opt.enabled)) {
        if (app.gdDir.empty()) showToast("Select your Geometry Dash folder below first.", true);
        saveOptions();
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    // ---- mode
    beginCard("mode");
    sectionTitle("Mode");
    {
        float gap = S(10), w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (modeCard("##extra", "Extrapolation", "Zero delay. Draws where you are right now, predicting at most half a physics tick (2 ms).", app.opt.mode == 0, w)) {
            app.opt.mode = 0;
            saveOptions();
        }
        ImGui::SameLine(0, gap);
        if (modeCard("##inter", "Interpolation", "Never guesses, so no tiny jumps on orbs or portals. Adds about 2 ms of visual delay.", app.opt.mode == 1, w)) {
            app.opt.mode = 1;
            saveOptions();
        }
    }
    endCard();

    // ---- options
    beginCard("options");
    sectionTitle("Options");
    bool ch = false;
    ch |= optionRow("Smart prediction", "Accounts for gravity and a slowing camera, and never predicts the camera going too far. Fixes small jumps when the camera moves up or down.", &app.opt.smart);
    ch |= optionRow("Smooth background and ground", "Applies the same smoothing to the background, ground and middleground so they don't shake against the camera.", &app.opt.background);
    ch |= optionRow("Smooth moving objects", "Also smooths blocks and decorations moved by Move, Rotate, Follow and Area triggers, so they don't shake against the camera. Only objects that are actually moving are touched.", &app.opt.objects);
    ch |= optionRow("Smooth trails", "Keeps the end of the trails (the normal one and the wave trail) attached to the icon. The game only extends them on physics steps, so between steps they ended a little behind the icon. Only the drawing is changed, for that frame.", &app.opt.trails);
    ch |= optionRow("Smooth rotation", "Smooths icon rotation when physics drives it (ship, wave...). The cube spin is already smooth and is left alone.", &app.opt.rotation);
    ch |= optionRow("Debug log", "Writes statistics to kfe.log in the Geometry Dash folder every 2 seconds.", &app.opt.debug);
    if (ch) saveOptions();
    endCard();

    // ---- explanation
    beginCard("about");
    sectionTitle("What does it do?");
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(
        "Geometry Dash moves everything in fixed physics steps, 240 times per second. Your monitor draws "
        "frames at its own rhythm, so most frames land between two steps and show a slightly old (or early) "
        "position. That is the tiny shaking you see on the camera and the icon.");
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextUnformatted(
        "Right before each frame is drawn, this mod moves the camera, the icons, the end of their trails, the "
        "background, the ground and objects moved by triggers "
        "to where they really are at that instant, and puts them back right after. It is visual only: physics, "
        "hitboxes and your inputs are never changed.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(6)));
    drawDiagram();
    endCard();

    // ---- install
    beginCard("install");
    sectionTitle("Installation");
    {
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::TextUnformatted("Geometry Dash folder:");
        ImGui::PopStyleColor();
        std::string dir = app.gdDir.empty() ? "(not found - click Browse)" : narrow(app.gdDir);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(dir.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(4)));
        if (button("Browse...", col::card2)) {
            std::wstring d = pickFolder(hwnd);
            if (!d.empty()) {
                if (isGdFolder(d)) setGdDir(d);
                else showToast("GeometryDash.exe is not in that folder.", true);
            }
        }
        ImGui::SameLine();
        bool canInstall = !app.gdDir.empty();
        if (button(app.installed ? "Reinstall / Update" : "Install", ImVec4(0.24f, 0.52f, 0.86f, 1), 0, canInstall)) doInstall();
        ImGui::SameLine();
        if (button("Uninstall", ImVec4(0.55f, 0.27f, 0.27f, 1), 0, app.installed || app.proxyIsOurs)) doUninstall();

        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::PushTextWrapPos(0);
        if (app.installed) {
            ImGui::TextColored(col::green, "Installed.");
        } else {
            ImGui::TextColored(col::dim, "Not installed.");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::TextUnformatted("Close Geometry Dash before installing or uninstalling. Works with or without Geode. "
            "Turning the mod on/off and changing options works instantly, even while playing.");
        ImGui::PopStyleColor();
        if (app.otherWinmm)
            ImGui::TextColored(col::yellow, "Another winmm.dll is in the game folder. Installing keeps a backup of it.");
        if (app.geodeVersionPresent)
            ImGui::TextColored(col::yellow, "The Geode version of this mod is installed. Install will disable it so it doesn't smooth twice.");
        ImGui::PopTextWrapPos();
    }
    endCard();

    // ---- footer / toast
    if (ImGui::GetTime() < app.toastUntil) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(app.toastColor, "%s", app.toast.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col::dim.x, col::dim.y, col::dim.z, 0.7f));
    ImGui::TextUnformatted("Visual only - physics and hitboxes are never changed.  Made by Kuang.");
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
    s.ScrollbarRounding = 8;
    s.ScrollbarSize = 12;
    s.ItemSpacing = ImVec2(10, 8);
    s.FramePadding = ImVec2(8, 5);
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = col::bg;
    c[ImGuiCol_ChildBg] = col::card;
    c[ImGuiCol_Text] = col::text;
    c[ImGuiCol_TextDisabled] = col::dim;
    c[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
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
    if (!g_fontBody) {
        ImFontConfig cfg;
        cfg.SizePixels = 15 * scale;
        g_fontBody = io.Fonts->AddFontDefault(&cfg);
    }
    g_fontBold = load("seguisb.ttf", 18);
    if (!g_fontBold) g_fontBold = g_fontBody;
    g_fontTitle = load("seguisb.ttf", 28);
    if (!g_fontTitle) g_fontTitle = g_fontBold;
    io.FontDefault = g_fontBody;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // remembered GD folder: %APPDATA%\KuangFrameExtrapolation\app.ini
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        std::wstring d = std::wstring(appdata) + L"\\KuangFrameExtrapolation";
        CreateDirectoryW(d.c_str(), nullptr);
        app.appIni = d + L"\\app.ini";
    }
    wchar_t saved[MAX_PATH] = {};
    GetPrivateProfileStringW(L"App", L"gd_folder", L"", saved, MAX_PATH, app.appIni.c_str());
    if (saved[0] && isGdFolder(saved)) setGdDir(saved);
    else {
        std::wstring found = findGdFolder();
        if (!found.empty()) setGdDir(found);
    }

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc, 0, 0, inst, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"KuangFrameExtrapolation", nullptr};
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowW(wc.lpszClassName, KFE_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(600, dpi, 96), MulDiv(860, dpi, 96), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = RGB(52, 53, 55);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));

    if (!createDevice(hwnd)) {
        cleanupDevice();
        MessageBoxW(nullptr, L"Could not start DirectX 11.", KFE_NAME, MB_ICONERROR);
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

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
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
        g_swapChain->Present(1, 0); // vsync: the app itself uses almost no CPU/GPU
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    return 0;
}
