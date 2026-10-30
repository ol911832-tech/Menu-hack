// Kuang Optimizer - control app (runs as administrator).
// Everything from GD Optimizer except the visual parts: low latency options inside the game
// (through kopt.dll), Windows tweaks, keyboard tools, restore points, parked apps and RAM purge.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "../shared/kfe_shared.h"
#include "kopt_shared.h"
#include "admin_shared.hpp"

// devguid.h only declares the device class GUIDs; define the one we use here
// (INITGUID can't be used: the Windows headers include devguid.h more than once).
extern "C" const GUID GUID_DEVCLASS_KEYBOARD = {0x4d36e96b, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

// ---------------------------------------------------------------------------
// D3D11 boilerplate (same as Kuang Frame Extrapolation)
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

// ---------------------------------------------------------------------------
// Helpers (same as Kuang Frame Extrapolation)
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
    const char* sub = *v ? "Click to turn the optimizer off" : "Click to turn the optimizer on";
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


// ---------------------------------------------------------------------------
// Texts (English texts from GD Optimizer, a few rewritten for this app)
// ---------------------------------------------------------------------------
struct SettingEn { const char* key; const char* name; const char* desc; };
#include "texts_en.inc"

struct TextOverride { const char* key; const char* desc; };
static const TextOverride kOverrides[] = {
    {"enabled", "Master switch for everything this app does inside Geometry Dash. Off: the game runs as if the optimizer was not installed. Permanent Windows tweaks are not affected by this switch."},
    {"io-priority", "High disk priority for the game. Windows only allows it from an administrator program: this app applies it from outside while it is open (it can stay in the tray)."},
    {"ram-purge", "Like ISLC: while the game is open, empties the memory standby list when both conditions below are met. Done by this app, so it has to stay open (it can stay in the tray)."},
    {"park-apps", "The programs you tick (Discord, the browser...) are kept on threads 0 and 1 only while Geometry Dash is open, so they do not use the game's cores. Also applies to ones you open later. When you untick them or close the game they use all cores again. Done by this app, so it has to stay open (it can stay in the tray)."},
    {"power-plan", "Power plan to use while Geometry Dash is open. The plan applies to all of Windows; when the game closes, the previous one is restored."},
    {"watchdog", "Every 2 seconds it checks CPU and GPU priority, cores, thread boost, timer and power plan inside the game, and puts them back if another program changed them. Runs on a lowest-priority thread pinned to core 0, so the game never waits for it. Only acts on options that are on."},
};

static const SettingEn* textFor(const char* key) {
    for (auto& e : kSettingsEn) if (std::strcmp(e.key, key) == 0) return &e;
    return nullptr;
}
static const char* nameOf(const char* key) { auto t = textFor(key); return t ? t->name : key; }
static const char* descOf(const char* key) {
    for (auto& o : kOverrides) if (std::strcmp(o.key, key) == 0) return o.desc;
    auto t = textFor(key);
    return t ? t->desc : "";
}

// ---------------------------------------------------------------------------
// Small Windows helpers
// ---------------------------------------------------------------------------
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::wstring appDataDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring d = std::wstring(buf) + L"\\KuangOptimizer";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::string guidText(const GUID& g) {
    char b[64];
    std::snprintf(b, sizeof(b), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
        g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

struct PowerPlan { GUID guid; std::string name; };
static std::vector<PowerPlan> powerPlans() {
    std::vector<PowerPlan> out;
    for (ULONG i = 0; i < 32; i++) {
        GUID guid;
        DWORD size = sizeof(guid);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, reinterpret_cast<UCHAR*>(&guid), &size) != ERROR_SUCCESS) break;
        wchar_t wname[256] = {};
        DWORD nsize = sizeof(wname);
        std::string name = "(no name)";
        if (PowerReadFriendlyName(nullptr, &guid, nullptr, nullptr, reinterpret_cast<UCHAR*>(wname), &nsize) == ERROR_SUCCESS) name = narrow(wname);
        out.push_back({guid, name});
    }
    return out;
}

// run a command line hidden, wait, capture output
static std::string runCapture(const std::wstring& cmdline, DWORD timeoutMs = 180000, DWORD* exitCode = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    std::string out;
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char buf[4096];
        DWORD got = 0;
        ULONGLONG start = GetTickCount64();
        while (true) {
            DWORD avail = 0;
            if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                if (ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got) out.append(buf, got);
                continue;
            }
            if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                       ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got)
                    out.append(buf, got);
                break;
            }
            if (GetTickCount64() - start > timeoutMs) { TerminateProcess(pi.hProcess, 1); break; }
        }
        if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return out;
}

static std::wstring systemExe(const wchar_t* rel) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(L"\"") + sys + L"\\" + rel + L"\"";
}

static std::string powershell(const std::wstring& script) {
    const unsigned char* data = reinterpret_cast<const unsigned char*>(script.data());
    std::string b64 = base64(data, script.size() * sizeof(wchar_t));
    return runCapture(systemExe(L"WindowsPowerShell\\v1.0\\powershell.exe") +
        L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + widen(b64));
}

static void enablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(token);
}

static std::string currentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::string out;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    if (len && GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        LPSTR s = nullptr;
        if (ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
    }
    CloseHandle(token);
    return out;
}

// ---------------------------------------------------------------------------
// Registry checks for the permanent tweaks (from GD Optimizer)
// ---------------------------------------------------------------------------
static bool splitRoot(const std::string& f, HKEY& root, std::string& sub) {
    auto pos = f.find('\\');
    if (pos == std::string::npos) return false;
    std::string r = f.substr(0, pos);
    sub = f.substr(pos + 1);
    if (r == "HKLM") root = HKEY_LOCAL_MACHINE;
    else if (r == "HKCU") root = HKEY_CURRENT_USER;
    else return false;
    return true;
}

static bool checkOne(const TweakCheck& c) {
    HKEY root; std::string sub;
    if (!splitRoot(fixNvPath(c.path), root, sub)) return false;
    HKEY h;
    if (RegOpenKeyExA(root, sub.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return c.kind == 'D'; // no key: Windows uses its default
    bool ok = false;
    if (c.kind == 'd' || c.kind == 'D') {
        DWORD v = 0, size = sizeof(v), type = 0;
        LONG res = RegQueryValueExA(h, c.name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size);
        if (res == ERROR_SUCCESS && type == REG_DWORD) ok = v == static_cast<DWORD>(std::strtoul(c.value, nullptr, 10));
        else if (c.kind == 'D' && res == ERROR_FILE_NOT_FOUND) ok = true;
    } else {
        char buf[1024] = {};
        DWORD size = sizeof(buf) - 1, type = 0;
        if (RegQueryValueExA(h, c.name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            std::string v = buf;
            ok = c.kind == 's' ? _stricmp(v.c_str(), c.value) == 0 : v.find(c.value) != std::string::npos;
        }
    }
    RegCloseKey(h);
    return ok;
}

static int tweakState(const WinTweak& t) {
    if (t.checks.empty()) return -1;
    for (auto& c : t.checks) if (!checkOne(c)) return 0;
    return 1;
}

static const WinTweak* findTweak(const char* key) {
    for (auto& t : kTweaks) if (std::strcmp(key, t.key) == 0) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// GeometryDash.exe compatibility flags (HKCU, per exe path)
// ---------------------------------------------------------------------------
static const char* kLayersKey = "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";

static std::vector<std::string> compatFlags(const std::wstring& exe) {
    std::vector<std::string> out;
    HKEY h;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kLayersKey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return out;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(h, exe.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS && type == REG_SZ) {
        std::string cur;
        for (char c : narrow(buf) + " ") {
            if (c == ' ') { if (!cur.empty() && cur != "~") out.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    RegCloseKey(h);
    return out;
}

static bool hasCompatFlag(const std::wstring& exe, const char* flag) {
    for (auto& f : compatFlags(exe)) if (_stricmp(f.c_str(), flag) == 0) return true;
    return false;
}

static bool setCompatFlag(const std::wstring& exe, const char* flag, bool on) {
    std::vector<std::string> next;
    for (auto& f : compatFlags(exe)) if (_stricmp(f.c_str(), flag) != 0) next.push_back(f);
    if (on) next.push_back(flag);
    HKEY h;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kLayersKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r;
    if (next.empty()) {
        r = RegDeleteValueW(h, exe.c_str());
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    } else {
        std::string text = "~";
        for (auto& f : next) text += " " + f;
        std::wstring w = widen(text);
        r = RegSetValueExW(h, exe.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()), static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keyboard / power plan settings (from GD Optimizer)
// ---------------------------------------------------------------------------
constexpr DWORD kFilterBits = FKF_FILTERKEYSON | FKF_HOTKEYACTIVE | FKF_CONFIRMHOTKEY;
constexpr DWORD kStickyBits = SKF_STICKYKEYSON | SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY;
constexpr DWORD kToggleBits = TKF_TOGGLEKEYSON | TKF_HOTKEYACTIVE | TKF_CONFIRMHOTKEY;
static const GUID kSubUsb = {0x2a737441, 0x1930, 0x4402, {0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3}};
static const GUID kUsbSuspend = {0x48e6b7a6, 0x50f5, 0x4782, {0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26}};
static const GUID kSubCpu = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
static const GUID kIdleMax = {0x9943e905, 0x9a30, 0x4ec1, {0x9b, 0x99, 0x44, 0xdd, 0x3b, 0x76, 0xf7, 0xa2}};
static const GUID kParkMin = {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}};

static bool pwrGet(const GUID& sub, const GUID& set, DWORD& v) {
    GUID* g = nullptr;
    if (PowerGetActiveScheme(nullptr, &g) != ERROR_SUCCESS || !g) return false;
    bool ok = PowerReadACValueIndex(nullptr, g, &sub, &set, &v) == ERROR_SUCCESS;
    LocalFree(g);
    return ok;
}
static bool pwrSet(const GUID& sub, const GUID& set, DWORD v) {
    GUID* g = nullptr;
    if (PowerGetActiveScheme(nullptr, &g) != ERROR_SUCCESS || !g) return false;
    bool ok = PowerWriteACValueIndex(nullptr, g, &sub, &set, v) == ERROR_SUCCESS;
    PowerSetActiveScheme(nullptr, g);
    LocalFree(g);
    return ok;
}
static bool cpuSleepRead(DWORD& idle, DWORD& park) { return pwrGet(kSubCpu, kIdleMax, idle) && pwrGet(kSubCpu, kParkMin, park); }
static bool cpuSleepLight(DWORD idle, DWORD park) { return idle == 1 && park == 100; }

struct KbdState { DWORD filter = 0, sticky = 0, toggle = 0; int delay = 1; DWORD speed = 31; bool ok = false; DWORD usb = 1; bool pwr = false; };
static KbdState kbdRead() {
    KbdState k;
    FILTERKEYS fk{}; fk.cbSize = sizeof(fk);
    STICKYKEYS sk{}; sk.cbSize = sizeof(sk);
    TOGGLEKEYS tk{}; tk.cbSize = sizeof(tk);
    k.ok = SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(fk), &fk, 0) && SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0) &&
        SystemParametersInfoW(SPI_GETTOGGLEKEYS, sizeof(tk), &tk, 0) && SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &k.delay, 0) &&
        SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &k.speed, 0);
    k.filter = fk.dwFlags; k.sticky = sk.dwFlags; k.toggle = tk.dwFlags;
    k.pwr = pwrGet(kSubUsb, kUsbSuspend, k.usb);
    return k;
}
static bool kbdAccFast(const KbdState& k) {
    return k.ok && !(k.filter & kFilterBits) && !(k.sticky & kStickyBits) && !(k.toggle & kToggleBits) && k.delay == 0 && k.speed == 31;
}
static bool kbdIsFast(const KbdState& k) { return kbdAccFast(k) && (!k.pwr || k.usb == 0); }
static void kbdWrite(DWORD filter, DWORD sticky, DWORD toggle, int delay, DWORD speed) {
    const UINT how = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    FILTERKEYS fk{}; fk.cbSize = sizeof(fk);
    if (SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(fk), &fk, 0)) { fk.dwFlags = filter; SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof(fk), &fk, how); }
    STICKYKEYS sk{}; sk.cbSize = sizeof(sk); sk.dwFlags = sticky;
    SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(sk), &sk, how);
    TOGGLEKEYS tk{}; tk.cbSize = sizeof(tk); tk.dwFlags = toggle;
    SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof(tk), &tk, how);
    SystemParametersInfoW(SPI_SETKEYBOARDDELAY, static_cast<UINT>(std::clamp(delay, 0, 3)), nullptr, how);
    SystemParametersInfoW(SPI_SETKEYBOARDSPEED, std::min<DWORD>(speed, 31), nullptr, how);
}

static int controllerMsi(const std::string& id) {
    if (!safeInstanceId(id)) return -1;
    DEVINST inst = 0;
    std::string tmp = id;
    if (CM_Locate_DevNodeA(&inst, tmp.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
        LOG_CONF conf = 0;
        if (CM_Get_First_Log_Conf(&conf, inst, ALLOC_LOG_CONF) == CR_SUCCESS) {
            int res = -1;
            RES_DES cur = 0, prev = 0;
            CONFIGRET r = CM_Get_Next_Res_Des(&cur, conf, ResType_IRQ, nullptr, 0);
            while (r == CR_SUCCESS) {
                ULONG size = 0;
                if (CM_Get_Res_Des_Data_Size(&size, cur, 0) == CR_SUCCESS && size >= sizeof(IRQ_DES) && size < 4096) {
                    std::vector<BYTE> buf(size);
                    if (CM_Get_Res_Des_Data(cur, buf.data(), size, 0) == CR_SUCCESS) {
                        auto d = reinterpret_cast<IRQ_DES*>(buf.data());
                        res = (d->IRQD_Alloc_Num & 0x80000000u) ? 1 : std::max(res, 0);
                    }
                }
                if (prev) CM_Free_Res_Des_Handle(prev);
                prev = cur;
                if (res == 1) break;
                r = CM_Get_Next_Res_Des(&cur, prev, ResType_IRQ, nullptr, 0);
            }
            if (prev) CM_Free_Res_Des_Handle(prev);
            CM_Free_Log_Conf_Handle(conf);
            if (res >= 0) return res;
        }
    }
    DWORD v = 0, sz = sizeof(v);
    std::string key = "SYSTEM\\CurrentControlSet\\Enum\\" + id + "\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key.c_str(), "MSISupported", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v ? 1 : 0;
    return -1;
}

static bool kbdCorePinned(const std::string& id) {
    DWORD pol = 0, sz = sizeof(pol);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kbdPolicyKey(id).c_str(), "DevicePolicy", RRF_RT_REG_DWORD, nullptr, &pol, &sz) != ERROR_SUCCESS || pol != 4) return false;
    BYTE b[16] = {}; sz = sizeof(b);
    return RegGetValueA(HKEY_LOCAL_MACHINE, kbdPolicyKey(id).c_str(), "AssignmentSetOverride", RRF_RT_REG_BINARY, nullptr, b, &sz) == ERROR_SUCCESS;
}

// one logical CPU per physical core, skipping core 0 (for the status text)
static DWORD_PTR wantedAffinity(DWORD_PTR systemMask) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return 0;
    std::vector<char> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) return 0;
    int best = 0;
    for (DWORD off = 0; off < len;) {
        auto cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (cur->Size == 0) break;
        if (cur->Relationship == RelationProcessorCore && cur->Processor.GroupCount > 0 && cur->Processor.GroupMask[0].Group == 0 &&
            (cur->Processor.GroupMask[0].Mask & systemMask))
            best = std::max(best, static_cast<int>(cur->Processor.EfficiencyClass));
        off += cur->Size;
    }
    DWORD_PTR mask = 0, maskAny = 0;
    for (DWORD off = 0; off < len;) {
        auto cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (cur->Size == 0) break;
        if (cur->Relationship == RelationProcessorCore && cur->Processor.GroupCount > 0 && cur->Processor.GroupMask[0].Group == 0) {
            DWORD_PTR core = cur->Processor.GroupMask[0].Mask & systemMask;
            if (core && !(core & 1)) {
                if (cur->Processor.Flags & LTP_PC_SMT) core = core & (~core + 1);
                maskAny |= core;
                if (static_cast<int>(cur->Processor.EfficiencyClass) == best) mask |= core;
            }
        }
        off += cur->Size;
    }
    return mask ? mask : maskAny;
}

static std::string maskList(uint64_t mask) {
    std::string out;
    for (int i = 0; i < 64; i++)
        if (mask & (1ull << i)) out += (out.empty() ? "" : ", ") + std::to_string(i);
    return out.empty() ? "none" : out;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
struct GameOptions {
    bool enabled = true;
    bool flushGpu = false, noVsync = false, highPriority = false, watchdog = true;
    bool gpuPriority = false, ioPriority = false, affinity = false, kbdCore = false;
    bool noBoost = false, timer = false;
    std::string powerPlan; // GUID text
};

struct RestorePoint { int seq = 0; std::string date, desc; };

struct App {
    std::wstring gdDir;
    std::wstring appIni;
    GameOptions opt;
    // only this app
    bool ramPurge = false;
    int ramStandbyMb = 1024, ramFreeMb = 1024;
    bool keepInTray = true;
    std::vector<std::string> parkNames;
    // install state
    bool installed = false, proxyIsOurs = false, otherWinmm = false;
    // game status
    bool running = false;
    KoptStatus status{};
    bool statusValid = false;
    double lastPoll = -10;
    // system state cache
    std::map<std::string, int> tweak;
    bool fso = false, dpi = false, kbdFast = false, cpuLight = false, kbdCoreOn = false, hasNvidia = false;
    std::vector<PowerPlan> plans;
    std::string kbdPathText, kbdMsiText;
    // jobs
    std::atomic<bool> busy{false};
    std::atomic<bool> refreshWanted{true};
    std::mutex jobMx;
    std::string jobResult;
    bool jobResultNew = false;
    // restore points
    std::vector<RestorePoint> points;
    int protection = -1;
    std::string restoreMsg;
    std::atomic<bool> restoreBusy{false};
    std::mutex restoreMx;
    // ui
    std::string toast;
    ImVec4 toastColor{1, 1, 1, 1};
    double toastUntil = 0;
    bool showPark = false, showMeter = false;
} app;

static std::wstring gdIni() { return withSlash(app.gdDir) + KOPT_INI_NAME; }
static std::wstring gdExe() { return withSlash(app.gdDir) + L"GeometryDash.exe"; }

static void showToast(const std::string& msg, bool error = false) {
    app.toast = msg;
    app.toastColor = error ? ImVec4(1.0f, 0.45f, 0.42f, 1) : ImVec4(0.45f, 0.86f, 0.6f, 1);
    app.toastUntil = ImGui::GetTime() + 6.0;
}

static void logLine(const std::string& s) {
    FILE* f = _wfopen((appDataDir() + L"\\tweaks.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, s.c_str());
    fclose(f);
}

// ---- app.ini (this app) ----
static int iniInt(const wchar_t* sec, const wchar_t* key, int def) { return GetPrivateProfileIntW(sec, key, def, app.appIni.c_str()); }
static void iniSetInt(const wchar_t* sec, const wchar_t* key, long long v) {
    WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), app.appIni.c_str());
}
static std::wstring iniStr(const wchar_t* sec, const wchar_t* key) {
    wchar_t buf[4096] = {};
    GetPrivateProfileStringW(sec, key, L"", buf, 4096, app.appIni.c_str());
    return buf;
}
static void iniSetStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), app.appIni.c_str());
}

static std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + sep) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    return out;
}
static std::string joinList(const std::vector<std::string>& v, char sep) {
    std::string out;
    for (auto& s : v) out += (out.empty() ? "" : std::string(1, sep)) + s;
    return out;
}

static void loadAppSettings() {
    app.ramPurge = iniInt(L"App", L"ram_purge", 0) != 0;
    app.ramStandbyMb = iniInt(L"App", L"ram_standby_mb", 1024);
    app.ramFreeMb = iniInt(L"App", L"ram_free_mb", 1024);
    app.keepInTray = iniInt(L"App", L"keep_in_tray", 1) != 0;
    app.parkNames = splitList(narrow(iniStr(L"App", L"park")), '|');
}
static void saveAppSettings() {
    iniSetInt(L"App", L"ram_purge", app.ramPurge);
    iniSetInt(L"App", L"ram_standby_mb", app.ramStandbyMb);
    iniSetInt(L"App", L"ram_free_mb", app.ramFreeMb);
    iniSetInt(L"App", L"keep_in_tray", app.keepInTray);
    iniSetStr(L"App", L"park", widen(joinList(app.parkNames, '|')));
}

// ---- kopt.ini (read live by the game) ----
static void loadGameOptions() {
    if (app.gdDir.empty()) return;
    auto p = gdIni();
    if (!fileExists(p)) return;
    auto geti = [&](const wchar_t* k, int d) { return GetPrivateProfileIntW(L"General", k, d, p.c_str()) != 0; };
    auto& o = app.opt;
    o.enabled = geti(L"enabled", 1);
    o.flushGpu = geti(L"flush_gpu", 0);
    o.noVsync = geti(L"no_vsync", 0);
    o.highPriority = geti(L"high_priority", 0);
    o.watchdog = geti(L"watchdog", 1);
    o.gpuPriority = geti(L"gpu_priority", 0);
    o.ioPriority = geti(L"io_priority", 0);
    o.affinity = geti(L"affinity", 0);
    o.kbdCore = geti(L"kbd_core", 0);
    o.noBoost = geti(L"no_boost", 0);
    o.timer = geti(L"timer", 0);
    wchar_t plan[64] = {};
    GetPrivateProfileStringW(L"General", L"power_plan", L"", plan, 64, p.c_str());
    o.powerPlan = narrow(plan);
}
static void saveGameOptions() {
    if (app.gdDir.empty()) return;
    auto p = gdIni();
    auto put = [&](const wchar_t* k, bool v) { WritePrivateProfileStringW(L"General", k, v ? L"1" : L"0", p.c_str()); };
    auto& o = app.opt;
    put(L"enabled", o.enabled);
    put(L"flush_gpu", o.flushGpu);
    put(L"no_vsync", o.noVsync);
    put(L"high_priority", o.highPriority);
    put(L"watchdog", o.watchdog);
    put(L"gpu_priority", o.gpuPriority);
    put(L"io_priority", o.ioPriority);
    put(L"affinity", o.affinity);
    put(L"kbd_core", o.kbdCore);
    put(L"no_boost", o.noBoost);
    put(L"timer", o.timer);
    WritePrivateProfileStringW(L"General", L"power_plan", widen(o.powerPlan).c_str(), p.c_str());
}

static void refreshInstallState() {
    app.installed = app.proxyIsOurs = app.otherWinmm = false;
    if (app.gdDir.empty()) return;
    std::wstring d = withSlash(app.gdDir);
    bool proxy = fileExists(d + KFE_PROXY_NAME);
    app.proxyIsOurs = proxy && fileContains(d + KFE_PROXY_NAME, KFE_PROXY_MARKER);
    app.otherWinmm = proxy && !app.proxyIsOurs;
    app.installed = app.proxyIsOurs && fileExists(d + KOPT_DLL_NAME);
}

static void setGdDir(const std::wstring& dir) {
    app.gdDir = dir;
    iniSetStr(L"App", L"gd_folder", dir);
    refreshInstallState();
    loadGameOptions();
    app.refreshWanted = true;
}

// read the real state of everything from Windows (slow-ish: called on demand)
static void refreshSystemState() {
    for (auto& t : kTweaks) app.tweak[t.key] = tweakState(t);
    if (!app.gdDir.empty()) {
        app.fso = hasCompatFlag(gdExe(), "DISABLEDXMAXIMIZEDWINDOWEDMODE");
        app.dpi = hasCompatFlag(gdExe(), "HIGHDPIAWARE");
    }
    KbdState k = kbdRead();
    app.kbdFast = k.ok && kbdIsFast(k);
    DWORD idle = 0, park = 100;
    app.cpuLight = cpuSleepRead(idle, park) && cpuSleepLight(idle, park);
    bool pinned = false;
    for (auto& d : splitList(narrow(iniStr(L"Keyboard", L"core_devs")), '|'))
        if (safeInstanceId(d) && kbdCorePinned(d)) pinned = true;
    app.kbdCoreOn = pinned;
    if (app.opt.kbdCore != pinned) { app.opt.kbdCore = pinned; saveGameOptions(); }
    app.hasNvidia = !nvidiaClassKey().empty();
    app.plans = powerPlans();
}

static void pollStatus() {
    double now = ImGui::GetTime();
    if (now - app.lastPoll < 0.5) return;
    app.lastPoll = now;
    app.running = gdRunning();
    app.statusValid = false;
    if (!app.running) return;
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, KOPT_STATUS_MAPPING);
    if (!h) return;
    if (auto v = static_cast<KoptStatus*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(KoptStatus)))) {
        app.status = *v;
        UnmapViewOfFile(v);
        app.statusValid = app.status.magic == KOPT_STATUS_MAGIC;
    }
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Background jobs: run elevated commands (this app runs as administrator)
// ---------------------------------------------------------------------------
static void runJob(std::vector<std::string> cmds, std::string doneMsg) {
    if (app.busy.exchange(true)) { showToast("Still applying the previous change, wait a moment.", true); return; }
    std::thread([cmds = std::move(cmds), doneMsg = std::move(doneMsg)] {
        int failed = 0;
        for (auto& c : cmds) {
            logLine("> " + c);
            DWORD code = 0;
            std::string out = runCapture(L"cmd.exe /d /c " + widen(c), 180000, &code);
            if (!out.empty()) logLine(out);
            if (code != 0) failed++;
        }
        {
            std::lock_guard lk(app.jobMx);
            app.jobResult = failed ? doneMsg + " (" + std::to_string(failed) + " step(s) reported an error, see tweaks.log)" : doneMsg;
            app.jobResultNew = true;
        }
        app.refreshWanted = true;
        app.busy = false;
    }).detach();
}

static void applyTweak(const WinTweak& t, bool on) {
    std::vector<std::string> cmds;
    for (const char* c : (on ? t.apply : t.revert)) {
        std::string cmd = c;
        if (cmd == "@lockmem-add" || cmd == "@lockmem-remove") {
            cmd = lockMemCommandFor(currentUserSid(), cmd == "@lockmem-add");
            if (cmd.empty()) continue;
        } else {
            cmd = fixNvPath(cmd);
        }
        cmds.push_back(cmd);
    }
    if (cmds.empty()) { showToast("Nothing to undo: that is the Windows default.", false); return; }
    runJob(cmds, std::string(nameOf(t.key)) + (on ? ": applied." : ": reverted."));
}

// ---------------------------------------------------------------------------
// Things done from outside while the game is open: I/O priority, parked apps, RAM purge
// ---------------------------------------------------------------------------
using NtSetInfo = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG);
using NtQueryInfo = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using NtSysQuery = LONG(NTAPI*)(int, PVOID, ULONG, PULONG);
using NtSysSet = LONG(NTAPI*)(int, PVOID, ULONG);

template <class T>
static T ntproc(const char* name) { return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)); }

static std::vector<std::pair<DWORD, std::string>> listProcs() {
    std::vector<std::pair<DWORD, std::string>> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::string n = narrow(pe.szExeFile);
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.push_back({pe.th32ProcessID, n});
    }
    CloseHandle(snap);
    return out;
}

struct Parked { std::string name; DWORD_PTR before; };
static std::map<DWORD, Parked> g_parked;

static void parkApply(bool active) {
    auto procs = listProcs();
    std::map<DWORD, std::string> alive(procs.begin(), procs.end());
    auto wanted = [&](const std::string& n) { return std::find(app.parkNames.begin(), app.parkNames.end(), n) != app.parkNames.end(); };
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        auto a = alive.find(it->first);
        if (a == alive.end() || a->second != it->second.name) { it = g_parked.erase(it); continue; }
        if (active && wanted(it->second.name)) { ++it; continue; }
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, it->first)) {
            DWORD_PTR pm = 0, sm = 0;
            if (GetProcessAffinityMask(h, &pm, &sm)) {
                DWORD_PTR back = it->second.before & sm;
                if (!back || back == (sm & 3)) back = sm;
                SetProcessAffinityMask(h, back);
            }
            CloseHandle(h);
        }
        it = g_parked.erase(it);
    }
    if (!active) return;
    DWORD self = GetCurrentProcessId();
    for (auto& [pid, name] : procs) {
        if (pid == self || pid == 0 || g_parked.count(pid) || !wanted(name)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
        if (!h) continue;
        DWORD_PTR pm = 0, sm = 0;
        if (GetProcessAffinityMask(h, &pm, &sm)) {
            DWORD_PTR target = sm & 3;
            if (target && (pm == target || SetProcessAffinityMask(h, target))) g_parked[pid] = {name, pm};
        }
        CloseHandle(h);
    }
}

struct MemoryListInfo {
    SIZE_T ZeroPageCount, FreePageCount, ModifiedPageCount, ModifiedNoWritePageCount, BadPageCount;
    SIZE_T PageCountByPriority[8];
    SIZE_T RepurposedPagesByPriority[8];
    SIZE_T ModifiedPageCountPageFile;
};
static ULONGLONG g_lastPurge = 0;
static int g_purges = 0;

static void maybePurge() {
    if (!app.ramPurge || app.ramStandbyMb <= 0) return;
    ULONGLONG now = GetTickCount64();
    if (g_lastPurge && now - g_lastPurge < 5000) return;
    auto query = ntproc<NtSysQuery>("NtQuerySystemInformation");
    auto set = ntproc<NtSysSet>("NtSetSystemInformation");
    if (!query || !set) return;
    MemoryListInfo info{};
    if (query(80, &info, sizeof(info), nullptr) != 0) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned long long page = si.dwPageSize, standby = 0;
    for (SIZE_T c : info.PageCountByPriority) standby += c;
    unsigned long long standbyMb = standby * page / (1024ull * 1024ull);
    unsigned long long freeMb = (static_cast<unsigned long long>(info.ZeroPageCount) + info.FreePageCount) * page / (1024ull * 1024ull);
    if (standbyMb >= static_cast<unsigned long long>(app.ramStandbyMb) && freeMb < static_cast<unsigned long long>(app.ramFreeMb)) {
        int cmd = 4; // MemoryPurgeStandbyList
        if (set(80, &cmd, sizeof(cmd)) == 0) g_purges++;
        g_lastPurge = now;
    }
}

static void externalIoPriority() {
    if (!app.opt.enabled || !app.opt.ioPriority) return;
    auto q = ntproc<NtQueryInfo>("NtQueryInformationProcess");
    auto s = ntproc<NtSetInfo>("NtSetInformationProcess");
    if (!q || !s) return;
    for (auto& [pid, name] : listProcs()) {
        if (name != "geometrydash.exe") continue;
        if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid)) {
            ULONG level = 0, got = 0;
            if (q(h, 33, &level, sizeof(level), &got) == 0 && level != 3) {
                ULONG want = 3;
                s(h, 33, &want, sizeof(want));
            }
            CloseHandle(h);
        }
    }
}

static ULONGLONG g_lastBackground = 0;
static void backgroundTick() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastBackground < 2000) return;
    g_lastBackground = now;
    bool gd = gdRunning();
    parkApply(gd && app.opt.enabled);
    if (gd) {
        externalIoPriority();
        maybePurge();
    }
}

// ---------------------------------------------------------------------------
// Restore points (PowerShell, from GD Optimizer's helper)
// ---------------------------------------------------------------------------
static const wchar_t* kRpHead =
    L"$k='HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore'\n"
    L"$msg=''\n$st='OK'\n$prot=0\n"
    L"try{if((Get-ItemProperty -Path $k -Name RPSessionInterval -ErrorAction Stop).RPSessionInterval -ne 0){$prot=1}}catch{}\n";
static const wchar_t* kRpCreate =
    L"$n='SystemRestorePointCreationFrequency'\n"
    L"$old=$null\ntry{$old=(Get-ItemProperty -Path $k -Name $n -ErrorAction Stop).$n}catch{}\n"
    L"try{Set-ItemProperty -Path $k -Name $n -Value 0 -Type DWord -ErrorAction Stop}catch{}\n"
    L"try{Checkpoint-Computer -Description 'Kuang Optimizer' -RestorePointType MODIFY_SETTINGS -WarningAction SilentlyContinue -ErrorAction Stop}catch{$msg='Could not create it: '+$_.Exception.Message}\n"
    L"try{if($null -eq $old){Remove-ItemProperty -Path $k -Name $n -ErrorAction Stop}else{Set-ItemProperty -Path $k -Name $n -Value $old -Type DWord -ErrorAction Stop}}catch{}\n";
static const wchar_t* kRpEnable = L"try{Enable-ComputerRestore -Drive \"$env:SystemDrive\\\" -ErrorAction Stop;$prot=1}catch{$msg='Could not turn it on: '+$_.Exception.Message}\n";
static const wchar_t* kRpList =
    L"$l=@()\n"
    L"try{Get-ComputerRestorePoint -ErrorAction Stop | ForEach-Object {"
    L"$d=[Management.ManagementDateTimeConverter]::ToDateTime($_.CreationTime).ToString('yyyy-MM-dd HH:mm');"
    L"$l+=('P|'+$_.SequenceNumber+'|'+$d+'|'+(($_.Description) -replace '[|\\r\\n]',' '))}}catch{$st='ERR';if($msg -eq ''){$msg=$_.Exception.Message}}\n"
    L"$st\n'PROT|'+$prot\n'MSG|'+($msg -replace '[\\r\\n]',' ')\n$l\n";

static void restoreJob(int kind, int seq = 0) { // 0 list, 1 create, 2 delete, 3 enable protection
    if (app.restoreBusy.exchange(true)) return;
    std::thread([kind, seq] {
        std::string extra;
        if (kind == 2) {
            using SRRemove = DWORD(WINAPI*)(DWORD);
            HMODULE sr = LoadLibraryExW(L"srclient.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            auto fn = sr ? reinterpret_cast<SRRemove>(GetProcAddress(sr, "SRRemoveRestorePoint")) : nullptr;
            DWORD r = fn ? fn(static_cast<DWORD>(seq)) : 1;
            extra = r == 0 ? "Restore point deleted." : "Could not delete it (code " + std::to_string(r) + ").";
        }
        std::wstring script = kRpHead;
        if (kind == 1) script += kRpCreate;
        if (kind == 3) script += kRpEnable;
        script += kRpList;
        std::string out = powershell(script);
        std::vector<RestorePoint> pts;
        int prot = -1;
        std::string msg;
        for (auto& line : splitList(out, '\n')) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.rfind("P|", 0) == 0) {
                auto f = splitList(l.substr(2), '|');
                if (f.size() >= 2) pts.push_back({std::atoi(f[0].c_str()), f[1], f.size() > 2 ? f[2] : ""});
            } else if (l.rfind("PROT|", 0) == 0) {
                prot = std::atoi(l.c_str() + 5);
            } else if (l.rfind("MSG|", 0) == 0) {
                msg = l.substr(4);
            }
        }
        std::reverse(pts.begin(), pts.end());
        if (kind == 1 && msg.empty()) msg = "Restore point created.";
        if (!extra.empty()) msg = extra;
        {
            std::lock_guard lk(app.restoreMx);
            app.points = pts;
            app.protection = prot;
            app.restoreMsg = msg;
        }
        app.restoreBusy = false;
    }).detach();
}

// ---------------------------------------------------------------------------
// Keyboard meter (from GD Optimizer)
// ---------------------------------------------------------------------------
namespace kbm {
    std::mutex mx;
    std::vector<double> times;
    bool down[256] = {};
    std::thread th;
    double freq = 1.0;
    std::atomic<bool> run{false};
    void start() {
        if (th.joinable()) return;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = static_cast<double>(f.QuadPart);
        { std::lock_guard lk(mx); times.clear(); }
        run = true;
        th = std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            for (int vk = 8; vk < 256; vk++) down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
            while (run) {
                for (int vk = 8; vk < 256; vk++) {
                    if (vk >= 0x10 && vk <= 0x12) continue;
                    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (d != down[vk]) {
                        down[vk] = d;
                        LARGE_INTEGER c; QueryPerformanceCounter(&c);
                        std::lock_guard lk(mx);
                        if (times.size() < 20000) times.push_back(c.QuadPart * 1000.0 / freq);
                    }
                }
                YieldProcessor();
            }
        });
    }
    void stop() { if (!th.joinable()) return; run = false; th.join(); }
    struct Stats { int n = 0; double period = 0, jitter = 0, worst = 0; };
    Stats compute() {
        std::vector<double> t;
        { std::lock_guard lk(mx); t = times; }
        std::vector<double> d;
        for (size_t i = 1; i < t.size(); i++) if (double x = t[i] - t[i - 1]; x >= 0 && x < 500.0) d.push_back(x);
        Stats s; s.n = static_cast<int>(d.size());
        if (s.n < 40) return s;
        for (double T : {8.0, 4.0, 2.0, 1.0}) {
            double sum = 0, worst = 0;
            for (double x : d) { double e = std::abs(x - std::round(x / T) * T); sum += e; worst = std::max(worst, e); }
            double mean = sum / d.size();
            if (mean / T < 0.15) { s.period = T; s.jitter = mean; s.worst = worst; break; }
        }
        return s;
    }
}

static std::string keyboardPathText() {
    auto paths = keyboardPaths();
    if (paths.empty()) return "No USB keyboard was found.";
    std::string out;
    bool anyHub = false;
    for (auto& p : paths) {
        out += (p.name.empty() ? std::string("USB device") : p.name.substr(0, 40)) + ": ";
        if (p.hubs == 0) out += "directly on a motherboard port.";
        else { anyHub = true; out += "goes through " + std::to_string(p.hubs) + " USB hub(s)."; }
        out += "\n";
    }
    if (anyHub) out += "If it is a hub, the monitor or the front panel, try a rear motherboard port.\n";
    if (paths.size() > 1) out += "Several show up because some mice and headsets also present themselves as keyboards.";
    return out;
}

static std::string keyboardMsiText() {
    auto devs = keyboardControllers();
    if (devs.empty()) return "Keyboard USB controller: no USB keyboard found.";
    int worst = 1;
    for (auto& d : devs) worst = std::min(worst, controllerMsi(d));
    if (worst == 1) return "Keyboard USB controller: MSI mode (correct).";
    if (worst == 0) return "Keyboard USB controller: legacy mode, no MSI.";
    return "Keyboard USB controller: could not check the mode.";
}

// ---------------------------------------------------------------------------
// Option handlers for the special (non-table) options
// ---------------------------------------------------------------------------
static void toggleKbdFast(bool want) {
    KbdState k = kbdRead();
    if (!k.ok) { showToast("Windows did not let us read the keyboard settings.", true); return; }
    std::vector<std::string> cmds;
    if (want) {
        if (!kbdAccFast(k) || !iniInt(L"Keyboard", L"prev_have", 0)) {
            iniSetInt(L"Keyboard", L"prev_filter", k.filter);
            iniSetInt(L"Keyboard", L"prev_sticky", k.sticky);
            iniSetInt(L"Keyboard", L"prev_toggle", k.toggle);
            iniSetInt(L"Keyboard", L"prev_delay", k.delay);
            iniSetInt(L"Keyboard", L"prev_speed", k.speed);
            iniSetInt(L"Keyboard", L"prev_have", 1);
        }
        kbdWrite(k.filter & ~kFilterBits, k.sticky & ~kStickyBits, k.toggle & ~kToggleBits, 0, 31);
        if (k.pwr && k.usb != 0) {
            iniSetInt(L"Keyboard", L"prev_usb", k.usb);
            pwrSet(kSubUsb, kUsbSuspend, 0);
        }
        auto ids = keyboardPathIds();
        iniSetStr(L"Keyboard", L"pm_devs", widen(joinList(ids, '|')));
        std::string cmd = kbdPmCommand(ids, false);
        if (!cmd.empty()) cmds.push_back(cmd);
    } else {
        pwrSet(kSubUsb, kUsbSuspend, iniInt(L"Keyboard", L"prev_usb", 1) ? 1 : 0);
        if (iniInt(L"Keyboard", L"prev_have", 0))
            kbdWrite(iniInt(L"Keyboard", L"prev_filter", 126), iniInt(L"Keyboard", L"prev_sticky", 510), iniInt(L"Keyboard", L"prev_toggle", 62),
                iniInt(L"Keyboard", L"prev_delay", 1), iniInt(L"Keyboard", L"prev_speed", 31));
        else
            kbdWrite(126, 510, 62, 1, 31);
        std::string cmd = kbdPmCommand(splitList(narrow(iniStr(L"Keyboard", L"pm_devs")), '|'), true);
        if (!cmd.empty()) cmds.push_back(cmd);
    }
    if (!cmds.empty()) runJob(cmds, want ? "Keyboard: Windows delays removed." : "Keyboard: previous settings restored.");
    else showToast(want ? "Keyboard: Windows delays removed." : "Keyboard: previous settings restored.");
    app.refreshWanted = true;
}

static void toggleCpuLight(bool want) {
    DWORD idle = 0, park = 100;
    if (!cpuSleepRead(idle, park)) { showToast("Windows did not let us read the power plan.", true); return; }
    bool ok;
    if (want) {
        iniSetInt(L"Cpu", L"prev_idle", idle);
        iniSetInt(L"Cpu", L"prev_park", park);
        ok = pwrSet(kSubCpu, kIdleMax, 1);
        ok = pwrSet(kSubCpu, kParkMin, 100) && ok;
    } else {
        ok = pwrSet(kSubCpu, kIdleMax, std::min<DWORD>(iniInt(L"Cpu", L"prev_idle", 0), 20));
        ok = pwrSet(kSubCpu, kParkMin, std::min<DWORD>(iniInt(L"Cpu", L"prev_park", 100), 100)) && ok;
    }
    showToast(ok ? (want ? "CPU: deep sleep removed." : "CPU: previous sleep settings restored.") : "Windows did not let us change the power plan.", !ok);
    app.refreshWanted = true;
}

static void toggleKbdCore(bool want) {
    std::vector<std::string> cmds;
    auto old = splitList(narrow(iniStr(L"Keyboard", L"core_devs")), '|');
    if (want) {
        DWORD_PTR pm = 0, sm = 0, full = 0, first = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
        if (!reservedCore(sm, full, first) || maskBits(wantedAffinity(sm) & ~full) < 2) {
            showToast("This CPU has too few cores to reserve one.", true);
            return;
        }
        auto devs = keyboardControllers();
        if (devs.empty()) { showToast("No USB keyboard was found.", true); return; }
        for (auto& d : old) if (safeInstanceId(d) && std::find(devs.begin(), devs.end(), d) == devs.end()) kbdCoreDelCmds(d, cmds);
        for (auto& d : devs) kbdCoreAddCmds(d, first, cmds);
        iniSetStr(L"Keyboard", L"core_devs", widen(joinList(devs, '|')));
        app.opt.kbdCore = true;
    } else {
        for (auto& d : old) if (safeInstanceId(d)) kbdCoreDelCmds(d, cmds);
        app.opt.kbdCore = false;
    }
    saveGameOptions();
    runJob(cmds, want ? "Keyboard core reserved. Restart the PC." : "Keyboard core given back to Windows. Restart the PC.");
}

static void toggleCompat(const char* flag, bool want) {
    if (app.gdDir.empty()) { showToast("Select your Geometry Dash folder first.", true); return; }
    bool ok = setCompatFlag(gdExe(), flag, want);
    showToast(ok ? "Saved. Applies the next time you open the game." : "Could not save it in the registry.", !ok);
    app.refreshWanted = true;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------
static bool doInstall() {
    if (app.gdDir.empty()) { showToast("Select your Geometry Dash folder first.", true); return false; }
    if (gdRunning()) { showToast("Close Geometry Dash first, then install.", true); return false; }
    std::wstring src = exeDir(), dst = withSlash(app.gdDir);
    if (!fileExists(src + KOPT_DLL_NAME) || !fileExists(src + KFE_PROXY_NAME)) {
        showToast("kopt.dll / winmm.dll are missing next to this app.", true);
        return false;
    }
    if (app.otherWinmm)
        MoveFileExW((dst + KFE_PROXY_NAME).c_str(), (dst + L"winmm.dll.kfe-backup").c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!CopyFileW((src + KOPT_DLL_NAME).c_str(), (dst + KOPT_DLL_NAME).c_str(), FALSE) ||
        !CopyFileW((src + KFE_PROXY_NAME).c_str(), (dst + KFE_PROXY_NAME).c_str(), FALSE)) {
        showToast("Could not copy the files.", true);
        refreshInstallState();
        return false;
    }
    saveGameOptions();
    refreshInstallState();
    showToast("Installed! Start Geometry Dash to use it.");
    return true;
}

static bool doUninstall() {
    if (gdRunning()) { showToast("Close Geometry Dash first, then uninstall.", true); return false; }
    std::wstring dst = withSlash(app.gdDir);
    bool ok = true;
    if (fileExists(dst + KOPT_DLL_NAME)) ok &= DeleteFileW((dst + KOPT_DLL_NAME).c_str()) != 0;
    // the loader is shared with Kuang Frame Extrapolation and Kuang Menu: keep it while one of them is installed
    if (app.proxyIsOurs && !fileExists(dst + KFE_DLL_NAME) && !fileExists(dst + L"kmenu.dll")) {
        ok &= DeleteFileW((dst + KFE_PROXY_NAME).c_str()) != 0;
        if (fileExists(dst + L"winmm.dll.kfe-backup"))
            MoveFileExW((dst + L"winmm.dll.kfe-backup").c_str(), (dst + KFE_PROXY_NAME).c_str(), 0);
    }
    refreshInstallState();
    showToast(ok ? "Uninstalled. Permanent Windows tweaks stay as they are (turn them off here first if you want)." : "Some files could not be removed.", !ok);
    return ok;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static bool header(const char* title, bool openByDefault = false) {
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushFont(g_fontBold);
    ImGui::PushStyleColor(ImGuiCol_Header, col::card);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col::card2);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, col::card2);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(9)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    bool open = ImGui::CollapsingHeader(title, openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    return open;
}

static void dimText(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// option stored in kopt.ini
static void gameOption(const char* key, bool* v) {
    if (optionRow(nameOf(key), descOf(key), v)) saveGameOptions();
}

static const char* restartTag(const char* key) {
    static const char* keys[] = {"w-hags", "w-mpo", "w-dynamic-tick", "w-vbs", "w-nv-power", "w-kbd-queue", "w-mouse-queue", "kbd-core"};
    for (auto k : keys) if (std::strcmp(k, key) == 0) return "  (restart required)";
    if (std::strcmp(key, "w-large-pages") == 0) return "  (sign out required)";
    return "";
}

// permanent tweak: the switch shows the real state read from Windows
static void tweakRow(const char* key) {
    const WinTweak* t = findTweak(key);
    if (!t) return;
    bool nv = std::strncmp(key, "w-nv-", 5) == 0;
    bool locked = (nv && !app.hasNvidia) || app.busy;
    int st = app.tweak.count(key) ? app.tweak[key] : -1;
    std::string label = std::string(nameOf(key)) + restartTag(key) + "##" + key;
    ImGui::BeginDisabled(locked);
    if (st < 0) {
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted((std::string(nameOf(key)) + restartTag(key)).c_str());
        ImGui::PopFont();
        if (button((std::string("Turn on##on") + key).c_str(), col::card2)) applyTweak(*t, true);
        ImGui::SameLine();
        if (button((std::string("Turn off##off") + key).c_str(), col::card2)) applyTweak(*t, false);
        dimText(descOf(key));
        ImGui::Dummy(ImVec2(0, S(4)));
    } else {
        bool v = st == 1;
        if (optionRow(label.c_str(), descOf(key), &v)) {
            if (!v && t->revert.empty()) showToast("That is already the Windows default value: nothing to undo.");
            else applyTweak(*t, v);
        }
    }
    ImGui::EndDisabled();
    if (nv && !app.hasNvidia) dimText("   (no NVIDIA graphics card found)");
}

static const char* priorityName(uint32_t c) {
    switch (c) {
        case HIGH_PRIORITY_CLASS: return "High";
        case ABOVE_NORMAL_PRIORITY_CLASS: return "Above normal";
        case NORMAL_PRIORITY_CLASS: return "Normal";
        case BELOW_NORMAL_PRIORITY_CLASS: return "Below normal";
        case IDLE_PRIORITY_CLASS: return "Low";
        case REALTIME_PRIORITY_CLASS: return "Realtime";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------
static void parkPopup() {
    if (app.showPark) { ImGui::OpenPopup("Programs on threads 0 and 1"); app.showPark = false; }
    ImGui::SetNextWindowSize(ImVec2(S(460), S(520)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Programs on threads 0 and 1", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    dimText("Tick the programs that should stay on threads 0 and 1 while Geometry Dash is open.");
    static std::vector<std::string> names;
    static double built = -10;
    if (ImGui::GetTime() - built > 3) {
        names.clear();
        for (auto& [pid, n] : listProcs()) {
            if (n == "geometrydash.exe" || n == "system" || n == "[system process]" || n == "registry" || n == "csrss.exe" ||
                n == "smss.exe" || n == "wininit.exe" || n == "services.exe" || n == "lsass.exe" || n == "kuangoptimizer.exe")
                continue;
            if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        }
        for (auto& n : app.parkNames) if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        std::sort(names.begin(), names.end());
        built = ImGui::GetTime();
    }
    ImGui::BeginChild("plist", ImVec2(0, -S(48)), ImGuiChildFlags_Borders);
    for (auto& n : names) {
        bool on = std::find(app.parkNames.begin(), app.parkNames.end(), n) != app.parkNames.end();
        if (ImGui::Checkbox(n.c_str(), &on)) {
            if (on) app.parkNames.push_back(n);
            else app.parkNames.erase(std::remove(app.parkNames.begin(), app.parkNames.end(), n), app.parkNames.end());
            saveAppSettings();
        }
    }
    ImGui::EndChild();
    if (button("Close", col::card2)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void meterPopup() {
    if (app.showMeter) { ImGui::OpenPopup("Keyboard meter"); app.showMeter = false; kbm::start(); }
    ImGui::SetNextWindowSize(ImVec2(S(480), S(0)), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Keyboard meter", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (kbm::run) kbm::stop();
        return;
    }
    dimText("Press many keys quickly (like rolling your fingers on the keyboard) for 10-20 seconds. "
        "It measures the real polling rate and the jitter of the path from USB to Windows. "
        "It only records when a key changes, not which key.");
    auto s = kbm::compute();
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::Text("Key events: %d", s.n);
    if (s.n < 40) {
        ImGui::TextColored(col::yellow, "Keep pressing keys...");
    } else if (s.period <= 0) {
        ImGui::TextColored(col::yellow, "No clear polling rate yet. Keep pressing keys.");
    } else {
        double hz = 1000.0 / s.period;
        if (s.period <= 1.0) ImGui::TextColored(col::green, "Polling rate: 1000 Hz or more");
        else ImGui::TextColored(col::green, "Polling rate: %.0f Hz", hz);
        ImGui::Text("Average jitter: %.2f ms   Worst: %.2f ms", s.jitter, s.worst);
        iniSetStr(L"Keyboard", L"last_meter", widen(std::to_string(static_cast<int>(hz)) + " Hz, jitter " + std::to_string(s.jitter).substr(0, 4) + " ms"));
    }
    std::string last = narrow(iniStr(L"Keyboard", L"last_meter"));
    if (!last.empty()) dimText(("Last measurement: " + last).c_str());
    ImGui::Dummy(ImVec2(0, S(6)));
    if (button("Restart", col::card2)) { kbm::stop(); kbm::start(); }
    ImGui::SameLine();
    if (button("Close", col::card2)) { kbm::stop(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
    if (!open) kbm::stop();
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
static void drawUI(HWND hwnd) {
    pollStatus();
    if (app.refreshWanted.exchange(false)) refreshSystemState();
    {
        std::lock_guard lk(app.jobMx);
        if (app.jobResultNew) { showToast(app.jobResult, app.jobResult.find("error") != std::string::npos); app.jobResultNew = false; }
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("Kuang Optimizer");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::Text("Lower latency and fewer hiccups for Geometry Dash 2.2091  -  v%s", KOPT_VERSION_STR);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(8)));

    // ---- status
    beginCard("status");
    {
        dotText(app.running ? col::green : col::dim, "Geometry Dash:", app.running ? "Running" : "Not running");
        const char* modText = app.installed ? "Installed - starts with the game" : "Not installed";
        ImVec4 modCol = app.installed ? col::accent : col::dim;
        char buf[256];
        bool live = app.running && app.statusValid && GetTickCount64() - app.status.heartbeat < 3000;
        if (app.running) {
            if (live) {
                if (app.status.state == KOPT_STATE_ERROR) {
                    std::snprintf(buf, sizeof(buf), "Partly active - %s", app.status.message);
                    modText = buf; modCol = col::yellow;
                } else if (!app.status.enabled) {
                    modText = "Loaded - turned off"; modCol = col::yellow;
                } else {
                    modText = app.status.state == KOPT_STATE_ACTIVE ? "Active" : "Active (GPU options start at the main menu)";
                    modCol = col::green;
                }
            } else if (app.installed) {
                modText = "Not loaded - restart Geometry Dash"; modCol = col::yellow;
            }
        }
        ImGui::PushTextWrapPos(0);
        dotText(modCol, "Optimizer:", modText);
        ImGui::PopTextWrapPos();
        if (live) {
            const KoptStatus& s = app.status;
            char l1[200], l2[200], l3[240];
            std::snprintf(l1, sizeof(l1), "%.0f FPS  -  CPU priority %s  -  timer %.2f ms", s.fps, priorityName(s.priorityClass), s.timerRes / 10000.0);
            std::snprintf(l2, sizeof(l2), "GPU priority %s  -  I/O priority %s  -  thread boost %s", s.gpuClass == 4 ? "High" : (s.gpuClass >= 0 ? "Normal" : "?"),
                s.ioLevel == 3 ? "High" : (s.ioLevel == 2 ? "Normal" : (s.ioLevel >= 0 ? "Low" : "?")), s.boostDisabled ? "off" : "on");
            std::string cores = s.affinity == s.systemAffinity ? std::string("all") : maskList(s.affinity);
            std::snprintf(l3, sizeof(l3), "Cores: %s  -  GPU queue %s  -  V-Sync %s  -  plan: %s  -  watchdog fixes: %u", cores.c_str(),
                s.flushGpu ? "flushed" : "normal", s.vsyncOff ? "forced off" : "game setting", s.plan, s.corrections);
            dotText(col::accent, "Live:", l1);
            ImGui::PushTextWrapPos(0);
            dotText(col::accent, "", l2);
            dotText(col::accent, "", l3);
            ImGui::PopTextWrapPos();
        }
        if (app.busy) dotText(col::yellow, "Windows:", "applying changes...");
    }
    endCard();

    if (bigToggle("##toggle", &app.opt.enabled)) {
        if (app.gdDir.empty()) showToast("Select your Geometry Dash folder below first.", true);
        saveGameOptions();
    }

    // ---- low latency
    if (header("Low latency", true)) {
        beginCard("lat");
        gameOption("flush-queue", &app.opt.flushGpu);
        gameOption("no-vsync", &app.opt.noVsync);
        gameOption("high-priority", &app.opt.highPriority);
        endCard();
    }

    // ---- while GD is open
    if (header("Windows (only while Geometry Dash is open)", true)) {
        beginCard("sys");
        gameOption("watchdog", &app.opt.watchdog);
        gameOption("gpu-priority", &app.opt.gpuPriority);
        gameOption("io-priority", &app.opt.ioPriority);
        gameOption("affinity", &app.opt.affinity);
        gameOption("no-boost", &app.opt.noBoost);
        gameOption("timer", &app.opt.timer);

        // power plan
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted(nameOf("power-plan"));
        ImGui::PopFont();
        std::string current = "Don't change";
        for (auto& p : app.plans) if (guidText(p.guid) == app.opt.powerPlan) current = p.name;
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::BeginCombo("##plan", current.c_str())) {
            if (ImGui::Selectable("Don't change", app.opt.powerPlan.empty())) { app.opt.powerPlan.clear(); saveGameOptions(); }
            for (auto& p : app.plans) {
                std::string g = guidText(p.guid);
                if (ImGui::Selectable((p.name + "##" + g).c_str(), g == app.opt.powerPlan)) { app.opt.powerPlan = g; saveGameOptions(); }
            }
            ImGui::EndCombo();
        }
        dimText(descOf("power-plan"));
        ImGui::Dummy(ImVec2(0, S(6)));

        // parked apps
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted(nameOf("park-apps"));
        ImGui::PopFont();
        std::string list = app.parkNames.empty() ? "(none)" : joinList(app.parkNames, ',');
        dimText(("Selected: " + list).c_str());
        if (button("Choose programs...", col::card2)) app.showPark = true;
        dimText(descOf("park-apps"));
        ImGui::Dummy(ImVec2(0, S(6)));

        // RAM purge
        if (optionRow(nameOf("ram-purge"), descOf("ram-purge"), &app.ramPurge)) saveAppSettings();
        ImGui::BeginDisabled(!app.ramPurge);
        ImGui::SetNextItemWidth(S(140));
        if (ImGui::InputInt("Standby list at least (MB)", &app.ramStandbyMb, 128, 1024)) { app.ramStandbyMb = std::max(0, app.ramStandbyMb); saveAppSettings(); }
        ImGui::SetNextItemWidth(S(140));
        if (ImGui::InputInt("...and free memory below (MB)", &app.ramFreeMb, 128, 1024)) { app.ramFreeMb = std::max(0, app.ramFreeMb); saveAppSettings(); }
        if (g_purges) ImGui::Text("Purges this session: %d", g_purges);
        ImGui::EndDisabled();
        endCard();
    }

    // ---- compatibility
    if (header("GeometryDash.exe compatibility")) {
        beginCard("compat");
        dimText(descOf("t-compat"));
        ImGui::Dummy(ImVec2(0, S(4)));
        bool v = app.fso;
        if (optionRow(nameOf("c-fso-off"), descOf("c-fso-off"), &v)) toggleCompat("DISABLEDXMAXIMIZEDWINDOWEDMODE", v);
        v = app.dpi;
        if (optionRow(nameOf("c-dpi-app"), descOf("c-dpi-app"), &v)) toggleCompat("HIGHDPIAWARE", v);
        endCard();
    }

    // ---- keyboard
    if (header("Keyboard")) {
        beginCard("kbd");
        ImGui::BeginDisabled(app.busy);
        bool v = app.kbdFast;
        if (optionRow(nameOf("kbd-fast"), descOf("kbd-fast"), &v)) toggleKbdFast(v);
        v = app.cpuLight;
        if (optionRow(nameOf("cpu-light-sleep"), descOf("cpu-light-sleep"), &v)) toggleCpuLight(v);
        ImGui::EndDisabled();
        tweakRow("w-kbd-queue");
        tweakRow("w-mouse-queue");
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted("Keyboard meter and connection");
        ImGui::PopFont();
        if (button("Keyboard meter...", col::card2)) app.showMeter = true;
        ImGui::SameLine();
        if (button("Check keyboard connection", col::card2)) { app.kbdPathText = keyboardPathText(); app.kbdMsiText = keyboardMsiText(); }
        if (!app.kbdPathText.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(app.kbdPathText.c_str());
            ImGui::TextUnformatted(app.kbdMsiText.c_str());
            ImGui::PopTextWrapPos();
        }
        dimText(descOf("kbd-path"));
        endCard();
    }

    // ---- permanent tweaks
    if (header("Permanent Windows tweaks")) {
        beginCard("perm");
        dimText("These change Windows itself (registry, services, boot) and stay after you close the game. "
            "Each switch shows the real state read from Windows. Create a restore point first (section below).");
        ImGui::Dummy(ImVec2(0, S(4)));
        for (const char* k : {"w-priority-sep", "w-power-throttling", "w-sys-responsiveness", "w-net-throttling", "w-game-dvr", "w-game-mode",
                 "w-games-task", "w-hags", "w-mpo", "w-large-pages", "w-telemetry", "w-activity", "w-advertising", "w-background-apps",
                 "w-nv-power", "w-nv-telemetry", "w-nv-tasks"})
            tweakRow(k);
        endCard();
    }

    // ---- advanced
    if (header("Advanced (risky)")) {
        beginCard("adv");
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(col::yellow,
            "Warning: these can lower security or break things if something goes wrong, and their benefit for "
            "input lag is debatable. Create a restore point before using them.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(4)));
        for (const char* k : {"w-dynamic-tick", "w-svc-sysmain", "w-svc-search", "w-svc-print", "w-svc-fax", "w-svc-diag", "w-vbs"}) tweakRow(k);
        ImGui::BeginDisabled(app.busy);
        bool v = app.kbdCoreOn;
        std::string lbl = std::string(nameOf("kbd-core")) + restartTag("kbd-core");
        if (optionRow(lbl.c_str(), descOf("kbd-core"), &v)) toggleKbdCore(v);
        ImGui::EndDisabled();
        endCard();
    }

    // ---- restore points
    if (header("Restore points")) {
        beginCard("rp");
        dimText("A restore point lets Windows undo system changes if something goes wrong. Create one before the permanent or advanced tweaks.");
        ImGui::Dummy(ImVec2(0, S(4)));
        bool rbusy = app.restoreBusy;
        ImGui::BeginDisabled(rbusy);
        if (button("Create restore point", ImVec4(0.24f, 0.52f, 0.86f, 1))) restoreJob(1);
        ImGui::SameLine();
        if (button("Refresh list", col::card2)) restoreJob(0);
        ImGui::EndDisabled();
        std::vector<RestorePoint> pts;
        int prot;
        std::string msg;
        {
            std::lock_guard lk(app.restoreMx);
            pts = app.points; prot = app.protection; msg = app.restoreMsg;
        }
        if (rbusy) ImGui::TextColored(col::yellow, "Working... (this can take a minute)");
        if (!msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(msg.c_str()); ImGui::PopTextWrapPos(); }
        if (prot == 0) {
            ImGui::TextColored(col::yellow, "System protection looks off, so restore points can't be created.");
            ImGui::BeginDisabled(rbusy);
            if (button("Turn on system protection", col::card2)) restoreJob(3);
            ImGui::EndDisabled();
        }
        for (auto& p : pts) {
            ImGui::PushID(p.seq);
            ImGui::Text("%s  -  %s", p.date.c_str(), p.desc.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(rbusy);
            if (ImGui::SmallButton("Delete")) restoreJob(2, p.seq);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (pts.empty() && !rbusy && prot != 0) dimText("Click \"Refresh list\" to see the existing restore points.");
        endCard();
    }

    // ---- installation
    if (header("Installation", !app.installed)) {
        beginCard("install");
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
        if (button(app.installed ? "Reinstall / Update" : "Install", ImVec4(0.24f, 0.52f, 0.86f, 1), 0, !app.gdDir.empty())) doInstall();
        ImGui::SameLine();
        if (button("Uninstall", ImVec4(0.55f, 0.27f, 0.27f, 1), 0, app.installed)) doUninstall();
        ImGui::PushTextWrapPos(0);
        if (app.installed) ImGui::TextColored(col::green, "Installed.");
        else ImGui::TextColored(col::dim, "Not installed.");
        dimText("Close Geometry Dash before installing or uninstalling. Uses the same loader as Kuang Frame Extrapolation; "
            "both can be installed at the same time. Options apply instantly, even while playing.");
        if (app.otherWinmm) ImGui::TextColored(col::yellow, "Another winmm.dll is in the game folder. Installing keeps a backup of it.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(4)));
        if (optionRow("Keep running in the tray when closed",
                "Closing the window hides it next to the clock instead of exiting. Needed for high I/O priority, "
                "parked programs and RAM purge, which this app does from outside the game.", &app.keepInTray))
            saveAppSettings();
        endCard();
    }

    if (ImGui::GetTime() < app.toastUntil) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(app.toastColor, "%s", app.toast.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col::dim.x, col::dim.y, col::dim.z, 0.7f));
    ImGui::TextUnformatted("Physics, hitboxes and inputs are never changed.  Made by Kuang.");
    ImGui::PopStyleColor();

    parkPopup();
    meterPopup();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Window, tray and main loop
// ---------------------------------------------------------------------------
constexpr UINT WM_TRAY = WM_APP + 1;
static NOTIFYICONDATAW g_tray{};
static bool g_hidden = false;
static bool g_quit = false;

static void trayAdd(HWND hwnd) {
    g_tray = {};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_tray.szTip, L"Kuang Optimizer", 64);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}
static void trayRemove() { Shell_NotifyIconW(NIM_DELETE, &g_tray); }

static void showFromTray(HWND hwnd) {
    trayRemove();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    g_hidden = false;
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
            mm->ptMinTrackSize.y = MulDiv(500, dpi, 96);
            return 0;
        }
        case WM_CLOSE:
            if (app.keepInTray && !g_quit) {
                ShowWindow(hwnd, SW_HIDE);
                trayAdd(hwnd);
                g_hidden = true;
                return 0;
            }
            break;
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) showFromTray(hwnd);
            else if (LOWORD(lp) == WM_RBUTTONUP) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, 1, L"Open Kuang Optimizer");
                AppendMenuW(m, MF_STRING, 2, L"Exit");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(m);
                if (cmd == 1) showFromTray(hwnd);
                if (cmd == 2) { g_quit = true; trayRemove(); DestroyWindow(hwnd); }
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            trayRemove();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &len) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // everything here needs administrator rights: ask Windows once and restart elevated
    if (!isElevated()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.lpVerb = L"runas";
        sei.lpFile = self;
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei))
            MessageBoxW(nullptr, L"Kuang Optimizer needs administrator rights to change Windows settings.", L"Kuang Optimizer", MB_ICONINFORMATION);
        return 0;
    }
    // only one copy (it can be hidden in the tray)
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\KuangOptimizerApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"KuangOptimizer", nullptr)) PostMessageW(w, WM_TRAY, 0, WM_LBUTTONUP);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    enablePrivilege(L"SeProfileSingleProcessPrivilege"); // RAM purge
    enablePrivilege(L"SeIncreaseBasePriorityPrivilege"); // I/O priority

    app.appIni = appDataDir() + L"\\app.ini";
    loadAppSettings();
    std::wstring saved = iniStr(L"App", L"gd_folder");
    if (saved.empty()) { // reuse the folder chosen in Kuang Frame Extrapolation
        wchar_t ad[MAX_PATH] = {};
        GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
        wchar_t buf[MAX_PATH] = {};
        GetPrivateProfileStringW(L"App", L"gd_folder", L"", buf, MAX_PATH, (std::wstring(ad) + L"\\KuangFrameExtrapolation\\app.ini").c_str());
        saved = buf;
    }
    if (!saved.empty() && isGdFolder(saved)) setGdDir(saved);
    else if (auto f = findGdFolder(); !f.empty()) setGdDir(f);

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc, 0, 0, inst, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"KuangOptimizer", nullptr};
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowW(wc.lpszClassName, KOPT_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(680, dpi, 96), MulDiv(900, dpi, 96), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF caption = RGB(52, 53, 55);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    if (!createDevice(hwnd)) {
        cleanupDevice();
        MessageBoxW(nullptr, L"Could not start DirectX 11.", KOPT_NAME, MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    g_scale = GetDpiForWindow(hwnd) / 96.0f;
    setupStyle(g_scale);
    loadFonts(g_scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    restoreJob(0);

    bool done = false;
    while (!done) {
        if (g_hidden) MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        backgroundTick();
        if (g_hidden || IsIconic(hwnd)) { if (!g_hidden) Sleep(50); continue; }
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
    parkApply(false); // give the parked programs their cores back
    kbm::stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    UnregisterClassW(wc.lpszClassName, inst);
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
