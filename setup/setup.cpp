// Kuang Setup - Geode-style installer wizard for the Kuang mods (Geometry Dash 2.2091).
// Pages: Welcome -> Game folder (auto-detected) -> Options -> Installing -> Finish.
// The mods are packed inside this .exe (resources 101-106). It also installs itself as KuangUninstall.exe and
// registers in "Installed apps" (HKCU, no admin needed). Run with --uninstall to remove everything.
// Command line: --uninstall | --gd "C:\path\to\Geometry Dash" | --silent (install with defaults, no UI)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "../shared/kfe_shared.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

#ifndef KUANG_SETUP_VERSION
#define KUANG_SETUP_VERSION "0.14.9"
#endif

// ------------------------------------------------------------------ language
static bool g_es = false;
static const wchar_t* T(const wchar_t* es, const wchar_t* en) { return g_es ? es : en; }

// ------------------------------------------------------------------ helpers
static std::wstring withSlash(std::wstring s) { if (!s.empty() && s.back() != L'\\' && s.back() != L'/') s += L'\\'; return s; }
static bool fileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static bool dirExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring exePath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }
static std::wstring exeDir() { std::wstring s = exePath(); return s.substr(0, s.find_last_of(L"\\/") + 1); }
static std::wstring parentDir(std::wstring d) {
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    size_t p = d.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : d.substr(0, p + 1);
}
static bool isGdFolder(const std::wstring& d) { return !d.empty() && fileExists(withSlash(d) + L"GeometryDash.exe"); }
static std::wstring trimSlash(std::wstring d) { while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back(); return d; }
static std::string narrowA(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring widenA(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static bool readFile(const std::wstring& path, std::string& out) {
    out.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
static std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[4096]; DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return {};
}
static void regSet(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static void regSetDword(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }

// Steam: registry + every library in libraryfolders.vdf; also common non-Steam places; also next to this exe
static std::wstring findGdFolder() {
    std::vector<std::wstring> libs;
    std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
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
                for (size_t i = 0; i < p.size(); i++) { if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++; clean += p[i]; }
                libs.push_back(widenA(clean));
                pos = b;
            }
        }
    }
    for (auto& l : libs) { std::wstring d = withSlash(l) + L"steamapps\\common\\Geometry Dash"; if (isGdFolder(d)) return d; }
    for (wchar_t drive = L'C'; drive <= L'H'; drive++) {
        const wchar_t* tails[] = {L":\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash", L":\\Program Files\\Steam\\steamapps\\common\\Geometry Dash",
                                  L":\\SteamLibrary\\steamapps\\common\\Geometry Dash", L":\\Games\\Geometry Dash", L":\\Geometry Dash"};
        for (auto t : tails) { std::wstring d = std::wstring(1, drive) + t; if (isGdFolder(d)) return d; }
    }
    for (std::wstring d = exeDir(); !d.empty(); d = parentDir(d)) if (isGdFolder(d)) return trimSlash(d);
    return {};
}

static std::wstring pickFolder(HWND owner, const wchar_t* title) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0; dlg->GetOptions(&opts);
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
    TOKEN_ELEVATION e{}; DWORD size = 0;
    bool r = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return r;
}

// ---- processes
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
static BOOL CALLBACK closeWindowsOf(HWND hwnd, LPARAM pid) {
    DWORD p = 0; GetWindowThreadProcessId(hwnd, &p);
    if (p == (DWORD)pid) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}
static bool closeProcess(const wchar_t* exe, int waitMs) {
    auto pids = findProcesses(exe);
    if (pids.empty()) return true;
    for (DWORD pid : pids) EnumWindows(closeWindowsOf, (LPARAM)pid);
    for (int t = 0; t < waitMs; t += 100) { if (findProcesses(exe).empty()) return true; Sleep(100); }
    for (DWORD pid : findProcesses(exe))
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) { TerminateProcess(h, 0); WaitForSingleObject(h, 3000); CloseHandle(h); }
    Sleep(300);
    return findProcesses(exe).empty();
}

// ------------------------------------------------------------------ components / payload
struct Component { const wchar_t* nameEs; const wchar_t* nameEn; const wchar_t* descEs; const wchar_t* descEn; const wchar_t* dll; const wchar_t* app; int dllRes; int appRes; };
static const Component kComps[] = {
    {L"Kuang Frame Extrapolation", L"Kuang Frame Extrapolation", L"Cámara, iconos y objetos fluidos a cualquier FPS.", L"Smooth camera, icons and moving objects at any FPS.",
     L"kfe.dll", L"KuangFrameExtrapolation.exe", 101, 105},
    {L"Kuang Optimizer", L"Kuang Optimizer", L"Menos input lag y ajustes de Windows mientras juegas.", L"Lower input lag and Windows tweaks while you play.",
     L"kopt.dll", L"KuangOptimizer.exe", 102, 106},
    {L"Kuang Menu", L"Kuang Menu", L"El menú dentro del juego (Tab): trucos, textos, CBF, trail...", L"The in-game mod menu (Tab): cheats, labels, CBF, custom trail...",
     L"kmenu.dll", nullptr, 103, 0},
};
constexpr int kNumComps = 3;
constexpr int kProxyRes = 104;

static bool loadRes(int id, const void*& data, DWORD& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    size = SizeofResource(nullptr, r);
    data = g ? LockResource(g) : nullptr;
    return data && size;
}
// writes a packed file and checks it byte by byte (retries: antivirus / the game may hold the file for a moment)
static bool writeRes(int id, const std::wstring& dst) {
    const void* data; DWORD size;
    if (!loadRes(id, data, size)) return false;
    for (int attempt = 0; attempt < 6; attempt++) {
        FILE* f = _wfopen(dst.c_str(), L"wb");
        if (f) {
            bool ok = fwrite(data, 1, size, f) == size;
            fclose(f);
            std::string back;
            if (ok && readFile(dst, back) && back.size() == size && memcmp(back.data(), data, size) == 0) return true;
        }
        Sleep(400);
    }
    return false;
}

// ------------------------------------------------------------------ state
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KuangMods";
static const wchar_t* kOwnKey = L"Software\\Kuang";

static std::wstring appsDir() {
    wchar_t la[MAX_PATH] = {}; GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    return std::wstring(la) + L"\\Programs\\Kuang\\";
}
static std::wstring desktopDir() { wchar_t p[MAX_PATH] = {}; SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, p); return p; }
static std::wstring startMenuDir() { wchar_t p[MAX_PATH] = {}; SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, p); return std::wstring(p) + L"\\Kuang"; }

struct State {
    bool uninstall = false, silent = false;
    std::wstring gdDir;
    bool want[kNumComps] = {true, true, true};
    bool desktopShortcut = true, startMenu = true, launchAfter = false;
    bool ok = false;
} st;

// worker -> UI
enum { WM_LOG = WM_APP + 1, WM_PROG = WM_APP + 2, WM_DONE = WM_APP + 3 };
static HWND g_hwnd = nullptr;
static void logLine(const std::wstring& s) { if (g_hwnd) PostMessageW(g_hwnd, WM_LOG, 0, (LPARAM) new std::wstring(s)); }
static void setProgress(int p) { if (g_hwnd) PostMessageW(g_hwnd, WM_PROG, p, 0); }

static void makeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args = L"") {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    link->SetPath(target.c_str());
    if (!args.empty()) link->SetArguments(args.c_str());
    link->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    link->Release();
}

static bool closeAll() {
    bool ok = true;
    for (auto e : kWatched) {
        if (findProcesses(e).empty()) continue;
        bool gd = _wcsicmp(e, L"GeometryDash.exe") == 0;
        logLine(std::wstring(T(L"Cerrando ", L"Closing ")) + e + (gd ? T(L" (guarda tu progreso primero)", L" (it saves your progress first)") : L""));
        if (!closeProcess(e, gd ? 12000 : 4000)) {
            logLine(std::wstring(T(L"ERROR: no se pudo cerrar ", L"ERROR: could not close ")) + e + T(L". Ciérralo y vuelve a intentarlo (si es el Optimizer, ejecuta este instalador como administrador).", L". Close it and try again (for the Optimizer, run this setup as administrator)."));
            ok = false;
        }
    }
    return ok;
}

static void registerUninstall(const std::wstring& dir) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        std::wstring un = dir + L"KuangUninstall.exe";
        regSet(k, L"DisplayName", L"Kuang Mods for Geometry Dash");
        regSet(k, L"DisplayVersion", widenA(KUANG_SETUP_VERSION));
        regSet(k, L"Publisher", L"Kuang");
        regSet(k, L"InstallLocation", st.gdDir);
        regSet(k, L"DisplayIcon", un);
        regSet(k, L"UninstallString", L"\"" + un + L"\" --uninstall");
        regSet(k, L"QuietUninstallString", L"\"" + un + L"\" --uninstall --silent");
        regSetDword(k, L"NoModify", 1);
        regSetDword(k, L"NoRepair", 1);
        regSetDword(k, L"EstimatedSize", 12000);
        RegCloseKey(k);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kOwnKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        regSet(k, L"GdDir", st.gdDir);
        regSet(k, L"AppsDir", dir);
        RegCloseKey(k);
    }
}

static void installWorker() {
    bool ok = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    setProgress(3);
    ok = closeAll();
    std::wstring gd = withSlash(st.gdDir), dir = appsDir();
    bool any = false;
    for (int i = 0; i < kNumComps; i++) any |= st.want[i];
    if (!any) { logLine(T(L"No hay nada seleccionado.", L"Nothing is selected.")); ok = false; }

    if (ok) {
        setProgress(15);
        logLine(std::wstring(T(L"Instalando en: ", L"Installing into: ")) + gd);
        int n = 0, total = 0;
        for (int i = 0; i < kNumComps; i++) total += st.want[i];
        for (int i = 0; i < kNumComps; i++) {
            const Component& c = kComps[i];
            std::wstring target = gd + c.dll;
            if (st.want[i]) {
                if (writeRes(c.dllRes, target)) logLine(std::wstring(L"OK  ") + (g_es ? c.nameEs : c.nameEn) + L"  (" + c.dll + L")");
                else { logLine(std::wstring(L"ERROR: ") + c.dll + T(L" no se pudo escribir (¿el juego sigue abierto o falta permiso?)", L" could not be written (is the game still open, or is permission missing?)")); ok = false; }
                setProgress(15 + 45 * ++n / total);
            } else if (fileExists(target)) {
                if (DeleteFileW(target.c_str())) logLine(std::wstring(T(L"Quitado (sin marcar): ", L"Removed (unchecked): ")) + c.dll);
            }
        }
        // shared loader winmm.dll (a foreign winmm.dll is kept as a backup)
        std::wstring proxy = gd + KFE_PROXY_NAME;
        std::string cur;
        bool ours = fileExists(proxy) && readFile(proxy, cur) && cur.find(KFE_PROXY_MARKER) != std::string::npos;
        if (fileExists(proxy) && !ours) {
            MoveFileExW(proxy.c_str(), (gd + L"winmm.dll.kfe-backup").c_str(), MOVEFILE_REPLACE_EXISTING);
            logLine(T(L"Había otro winmm.dll: guardado como winmm.dll.kfe-backup.", L"Another winmm.dll was there: kept as winmm.dll.kfe-backup."));
        }
        if (writeRes(kProxyRes, proxy)) logLine(T(L"OK  Cargador (winmm.dll)", L"OK  Loader (winmm.dll)"));
        else { logLine(T(L"ERROR: winmm.dll no se pudo escribir.", L"ERROR: winmm.dll could not be written.")); ok = false; }
        // the old Geode build of Frame Extrapolation would smooth twice
        std::wstring geodeMod = gd + L"geode\\mods\\kuang.frame-extrapolation.geode";
        if (st.want[0] && fileExists(geodeMod)) {
            MoveFileExW(geodeMod.c_str(), (geodeMod + L".disabled").c_str(), MOVEFILE_REPLACE_EXISTING);
            logLine(T(L"Desactivada la versión Geode antigua de Frame Extrapolation.", L"Disabled the old Geode version of Frame Extrapolation."));
        }
    }
    if (ok) {
        setProgress(65);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        // programs + a copy of the DLLs next to them (their own Install buttons copy from there)
        for (int i = 0; i < kNumComps; i++) {
            if (!st.want[i]) continue;
            writeRes(kComps[i].dllRes, dir + kComps[i].dll);
            if (kComps[i].app) {
                if (writeRes(kComps[i].appRes, dir + kComps[i].app)) logLine(std::wstring(T(L"Programa listo: ", L"Program ready: ")) + kComps[i].app);
                else logLine(std::wstring(T(L"Aviso: no se pudo copiar ", L"Warning: could not copy ")) + kComps[i].app);
            }
        }
        writeRes(kProxyRes, dir + L"winmm.dll");
        // uninstaller = a copy of this exe
        std::wstring un = dir + L"KuangUninstall.exe";
        if (_wcsicmp(exePath().c_str(), un.c_str()) != 0 && !CopyFileW(exePath().c_str(), un.c_str(), FALSE))
            logLine(T(L"Aviso: no se pudo crear el desinstalador.", L"Warning: could not create the uninstaller."));
        setProgress(80);
        registerUninstall(dir);
        logLine(T(L"Registrado en «Aplicaciones instaladas» (desinstalar desde ahí).", L"Registered in “Installed apps” (uninstall from there)."));
        if (st.desktopShortcut)
            for (int i = 0; i < kNumComps; i++)
                if (st.want[i] && kComps[i].app) makeShortcut(desktopDir() + L"\\" + (g_es ? kComps[i].nameEs : kComps[i].nameEn) + L".lnk", dir + kComps[i].app);
        if (st.startMenu) {
            std::wstring sm = startMenuDir();
            SHCreateDirectoryExW(nullptr, sm.c_str(), nullptr);
            for (int i = 0; i < kNumComps; i++)
                if (st.want[i] && kComps[i].app) makeShortcut(sm + L"\\" + (g_es ? kComps[i].nameEs : kComps[i].nameEn) + L".lnk", dir + kComps[i].app);
            makeShortcut(sm + L"\\" + T(L"Desinstalar Kuang", L"Uninstall Kuang") + L".lnk", un, L"--uninstall");
        }
        setProgress(100);
        logLine(T(L"Todo instalado y comprobado. Abre Geometry Dash y pulsa Tab.", L"Everything is installed and checked. Open Geometry Dash and press Tab."));
    }
    CoUninitialize();
    PostMessageW(g_hwnd, WM_DONE, ok ? 1 : 0, 0);
}

static void uninstallWorker() {
    bool ok = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    setProgress(5);
    ok = closeAll();
    if (ok) {
        std::wstring gd = withSlash(st.gdDir), dir = appsDir();
        bool haveGd = !st.gdDir.empty();   // never touch relative paths when the game folder is unknown
        setProgress(30);
        for (int i = 0; haveGd && i < kNumComps; i++) {
            std::wstring f = gd + kComps[i].dll;
            if (!fileExists(f)) continue;
            if (DeleteFileW(f.c_str())) logLine(std::wstring(T(L"Quitado ", L"Removed ")) + kComps[i].dll);
            else { logLine(std::wstring(L"ERROR: ") + kComps[i].dll); ok = false; }
        }
        std::wstring proxy = gd + KFE_PROXY_NAME;
        std::string cur;
        if (haveGd && fileExists(proxy) && readFile(proxy, cur) && cur.find(KFE_PROXY_MARKER) != std::string::npos) {
            if (DeleteFileW(proxy.c_str())) logLine(T(L"Quitado el cargador (winmm.dll)", L"Removed the loader (winmm.dll)"));
            else { logLine(L"ERROR: winmm.dll"); ok = false; }
            if (fileExists(gd + L"winmm.dll.kfe-backup")) MoveFileExW((gd + L"winmm.dll.kfe-backup").c_str(), proxy.c_str(), 0);
        }
        setProgress(65);
        const wchar_t* files[] = {L"kfe.dll", L"kopt.dll", L"kmenu.dll", L"winmm.dll", L"KuangFrameExtrapolation.exe", L"KuangOptimizer.exe"};
        for (auto f : files) DeleteFileW((dir + f).c_str());
        for (int i = 0; i < kNumComps; i++) if (kComps[i].app) {
            DeleteFileW((desktopDir() + L"\\" + kComps[i].nameEs + L".lnk").c_str());
            DeleteFileW((desktopDir() + L"\\" + kComps[i].nameEn + L".lnk").c_str());
        }
        std::wstring sm = startMenuDir();
        if (dirExists(sm)) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((sm + L"\\*.lnk").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) { do DeleteFileW((sm + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(h, &fd)); FindClose(h); }
            RemoveDirectoryW(sm.c_str());
        }
        RegDeleteKeyW(HKEY_CURRENT_USER, kUninstKey);
        RegDeleteKeyW(HKEY_CURRENT_USER, kOwnKey);
        logLine(T(L"Tus ajustes (kfe.ini, kopt.ini, kmenu.ini) se conservan.", L"Your settings (kfe.ini, kopt.ini, kmenu.ini) were kept."));
        setProgress(100);
        // this exe lives in dir: let a helper delete it once we have exited
        std::wstring un = dir + L"KuangUninstall.exe";
        if (_wcsicmp(exePath().c_str(), un.c_str()) == 0) {
            std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + un + L"\" & rmdir \"" + trimSlash(dir) + L"\"";
            STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        }
    }
    CoUninitialize();
    PostMessageW(g_hwnd, WM_DONE, ok ? 1 : 0, 0);
}

// ------------------------------------------------------------------ UI
enum Page { P_WELCOME, P_FOLDER, P_OPTIONS, P_PROGRESS, P_FINISH };
enum {
    ID_BACK = 100, ID_NEXT, ID_CANCEL, ID_BROWSE, ID_PATH, ID_COMP0 = 110, ID_DESK = 120, ID_START, ID_LAUNCH, ID_LOG, ID_PBAR
};
static Page g_page = P_WELCOME;
static HFONT g_font, g_fontBig, g_fontTitle, g_fontSmall;
static HWND h_back, h_next, h_cancel, h_browse, h_path, h_pathStatus, h_log, h_pbar, h_stepText, h_launch, h_desk, h_start, h_finishText;
static HWND h_comp[kNumComps], h_compDesc[kNumComps], h_pageTitle, h_pageText;
static bool g_busy = false, g_failed = false;
static double g_scale = 1.0;
static int S(int v) { return (int)(v * g_scale + 0.5); }
constexpr COLORREF kSideBg = RGB(24, 22, 38), kAccent = RGB(167, 139, 250), kBodyBg = RGB(250, 250, 252);
static HBRUSH g_bodyBrush;

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, HFONT f = nullptr, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

static std::vector<Page> pages() {
    if (st.uninstall) return {P_WELCOME, P_PROGRESS, P_FINISH};
    return {P_WELCOME, P_FOLDER, P_OPTIONS, P_PROGRESS, P_FINISH};
}
static std::wstring pageName(Page p) {
    switch (p) {
        case P_WELCOME: return st.uninstall ? T(L"Desinstalar", L"Uninstall") : T(L"Bienvenida", L"Welcome");
        case P_FOLDER: return T(L"Carpeta del juego", L"Game folder");
        case P_OPTIONS: return T(L"Opciones", L"Options");
        case P_PROGRESS: return st.uninstall ? T(L"Desinstalando", L"Uninstalling") : T(L"Instalando", L"Installing");
        default: return T(L"Listo", L"Finish");
    }
}

static void updateFolderStatus() {
    wchar_t buf[1024]; GetWindowTextW(h_path, buf, 1024);
    st.gdDir = trimSlash(buf);
    bool good = isGdFolder(st.gdDir);
    SetWindowTextW(h_pathStatus, good ? T(L"✔ Geometry Dash encontrado en esta carpeta.", L"✔ Geometry Dash found in this folder.")
                                       : T(L"✘ Aquí no está GeometryDash.exe. Elige la carpeta del juego.", L"✘ GeometryDash.exe is not here. Pick the game folder."));
    EnableWindow(h_next, good);
}

static void showPage(Page p) {
    g_page = p;
    auto show = [&](std::initializer_list<HWND> l, bool v) { for (HWND h : l) ShowWindow(h, v ? SW_SHOW : SW_HIDE); };
    show({h_pageTitle, h_pageText, h_path, h_browse, h_pathStatus, h_desk, h_start, h_log, h_pbar, h_stepText, h_launch, h_finishText}, false);
    for (int i = 0; i < kNumComps; i++) show({h_comp[i], h_compDesc[i]}, false);
    SetWindowTextW(h_next, T(L"Siguiente >", L"Next >"));
    EnableWindow(h_next, TRUE); EnableWindow(h_back, p != P_WELCOME && p != P_PROGRESS && p != P_FINISH);
    EnableWindow(h_cancel, TRUE);
    SetWindowTextW(h_cancel, T(L"Cancelar", L"Cancel"));
    show({h_pageTitle}, true);
    switch (p) {
        case P_WELCOME:
            SetWindowTextW(h_pageTitle, st.uninstall ? T(L"Desinstalar Kuang", L"Uninstall Kuang") : T(L"Bienvenido a Kuang", L"Welcome to Kuang"));
            SetWindowTextW(h_pageText, st.uninstall
                ? T(L"Se quitarán los mods Kuang de la carpeta de Geometry Dash, los programas, los accesos directos y el registro en «Aplicaciones instaladas».\r\n\r\nTus ajustes (kfe.ini, kopt.ini, kmenu.ini) se conservan.",
                    L"This removes the Kuang mods from the Geometry Dash folder, the programs, the shortcuts and the entry in “Installed apps”.\r\n\r\nYour settings (kfe.ini, kopt.ini, kmenu.ini) are kept.")
                : T(L"Este asistente instala Kuang para Geometry Dash 2.2091:\r\n\r\n  •  Kuang Menu: el menú dentro del juego (Tab)\r\n  •  Kuang Frame Extrapolation\r\n  •  Kuang Optimizer\r\n\r\nBuscará tu carpeta del juego automáticamente. No necesitas Geode.",
                    L"This wizard installs Kuang for Geometry Dash 2.2091:\r\n\r\n  •  Kuang Menu: the in-game menu (Tab)\r\n  •  Kuang Frame Extrapolation\r\n  •  Kuang Optimizer\r\n\r\nIt finds your game folder automatically. You do not need Geode."));
            show({h_pageText}, true);
            SetWindowTextW(h_next, st.uninstall ? T(L"Desinstalar", L"Uninstall") : T(L"Siguiente >", L"Next >"));
            break;
        case P_FOLDER:
            SetWindowTextW(h_pageTitle, T(L"Carpeta de Geometry Dash", L"Geometry Dash folder"));
            SetWindowTextW(h_pageText, T(L"Los mods se instalan en la carpeta donde está GeometryDash.exe. Si no es la correcta, pulsa «Examinar».", L"The mods go into the folder that holds GeometryDash.exe. If this is not the right one, press “Browse”."));
            show({h_pageText, h_path, h_browse, h_pathStatus}, true);
            updateFolderStatus();
            break;
        case P_OPTIONS:
            SetWindowTextW(h_pageTitle, T(L"Qué instalar", L"What to install"));
            for (int i = 0; i < kNumComps; i++) show({h_comp[i], h_compDesc[i]}, true);
            show({h_desk, h_start}, true);
            SetWindowTextW(h_next, T(L"Instalar", L"Install"));
            break;
        case P_PROGRESS:
            SetWindowTextW(h_pageTitle, st.uninstall ? T(L"Desinstalando...", L"Uninstalling...") : T(L"Instalando...", L"Installing..."));
            show({h_log, h_pbar, h_stepText}, true);
            EnableWindow(h_next, FALSE); EnableWindow(h_back, FALSE); EnableWindow(h_cancel, FALSE);
            SetWindowTextW(h_log, L"");
            SendMessageW(h_pbar, PBM_SETPOS, 0, 0);
            g_busy = true; g_failed = false;
            std::thread(st.uninstall ? uninstallWorker : installWorker).detach();
            break;
        case P_FINISH:
            SetWindowTextW(h_pageTitle, !g_failed ? (st.uninstall ? T(L"Desinstalado", L"Uninstalled") : T(L"¡Instalación completa!", L"Installation complete!"))
                                                  : T(L"Algo falló", L"Something failed"));
            SetWindowTextW(h_finishText, !g_failed
                ? (st.uninstall ? T(L"Kuang se quitó de Geometry Dash.", L"Kuang was removed from Geometry Dash.")
                                : T(L"Abre Geometry Dash y pulsa Tab para el menú de Kuang.\r\n\r\nPara desinstalar: Configuración › Aplicaciones › Aplicaciones instaladas › Kuang Mods.", L"Open Geometry Dash and press Tab for the Kuang menu.\r\n\r\nTo uninstall: Settings › Apps › Installed apps › Kuang Mods."))
                : T(L"Lee las líneas con ERROR del registro de abajo. Si el juego estaba abierto, ciérralo y vuelve a intentarlo. Si dice falta de permiso, ejecuta el instalador como administrador.", L"Read the ERROR lines in the log below. If the game was open, close it and try again. If it is a permission problem, run the setup as administrator."));
            show({h_finishText, h_log}, true);
            if (!g_failed && !st.uninstall) show({h_launch}, true);
            SetWindowTextW(h_next, T(L"Finalizar", L"Finish"));
            EnableWindow(h_back, FALSE);
            EnableWindow(h_cancel, FALSE);
            break;
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

static void gotoStep(int delta) {
    auto pg = pages();
    auto it = std::find(pg.begin(), pg.end(), g_page);
    if (it == pg.end()) return;
    int idx = (int)(it - pg.begin()) + delta;
    if (idx < 0 || idx >= (int)pg.size()) return;
    showPage(pg[idx]);
}

static void collectOptions() {
    for (int i = 0; i < kNumComps; i++) st.want[i] = SendMessageW(h_comp[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.desktopShortcut = SendMessageW(h_desk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.startMenu = SendMessageW(h_start, BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.launchAfter = SendMessageW(h_launch, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void relaunchAsAdmin() {
    std::wstring args = st.uninstall ? L"--uninstall" : L"";
    if (!st.gdDir.empty()) args += L" --gd \"" + st.gdDir + L"\"";
    ShellExecuteW(g_hwnd, L"runas", exePath().c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void paintSide(HDC dc, RECT rc) {
    RECT side{0, 0, S(200), rc.bottom};
    HBRUSH b = CreateSolidBrush(kSideBg); FillRect(dc, &side, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontTitle); SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, S(24), S(28), L"Kuang", 5);
    SelectObject(dc, g_fontSmall); SetTextColor(dc, RGB(160, 155, 190));
    std::wstring sub = std::wstring(T(L"para Geometry Dash 2.2091  ·  v", L"for Geometry Dash 2.2091  ·  v")) + widenA(KUANG_SETUP_VERSION);
    RECT sr{S(24), S(72), S(190), S(110)}; DrawTextW(dc, sub.c_str(), -1, &sr, DT_WORDBREAK);
    int y = S(150);
    for (Page p : pages()) {
        bool cur = p == g_page;
        SelectObject(dc, g_font);
        SetTextColor(dc, cur ? kAccent : RGB(120, 116, 150));
        std::wstring line = (cur ? L"●  " : L"○  ") + pageName(p);
        TextOutW(dc, S(24), y, line.c_str(), (int)line.size());
        y += S(30);
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp; RECT rc; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, g_bodyBrush);
            paintSide(dc, rc);
            return 1;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, kBodyBg);
            HWND c = (HWND)lp;
            SetTextColor(dc, c == h_pageTitle ? RGB(24, 22, 38) : (c == h_pathStatus ? (isGdFolder(st.gdDir) ? RGB(20, 130, 60) : RGB(190, 40, 40)) : RGB(60, 60, 75)));
            return (LRESULT)g_bodyBrush;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_PATH && HIWORD(wp) == EN_CHANGE && g_page == P_FOLDER) { updateFolderStatus(); return 0; }
            if (id == ID_BROWSE) {
                std::wstring p = pickFolder(hwnd, T(L"Elige la carpeta de Geometry Dash", L"Pick the Geometry Dash folder"));
                if (!p.empty()) { SetWindowTextW(h_path, p.c_str()); updateFolderStatus(); }
                return 0;
            }
            if (id == ID_BACK) { gotoStep(-1); return 0; }
            if (id == ID_CANCEL) { if (!g_busy) DestroyWindow(hwnd); return 0; }
            if (id == ID_NEXT) {
                if (g_page == P_FINISH) {
                    collectOptions();
                    if (g_failed && !isElevated() && MessageBoxW(hwnd, T(L"¿Reintentar como administrador?", L"Retry as administrator?"), L"Kuang", MB_YESNO | MB_ICONQUESTION) == IDYES) { relaunchAsAdmin(); DestroyWindow(hwnd); return 0; }
                    if (!g_failed && !st.uninstall && st.launchAfter) ShellExecuteW(nullptr, L"open", L"steam://rungameid/322170", nullptr, nullptr, SW_SHOWNORMAL);
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (g_page == P_OPTIONS) {
                    collectOptions();
                    bool any = false; for (bool w : st.want) any |= w;
                    if (!any) { MessageBoxW(hwnd, T(L"Marca al menos un componente.", L"Tick at least one component."), L"Kuang", MB_ICONINFORMATION); return 0; }
                }
                gotoStep(1);
                return 0;
            }
            break;
        }
        case WM_LOG: {
            std::wstring* s = (std::wstring*)lp;
            int len = GetWindowTextLengthW(h_log);
            SendMessageW(h_log, EM_SETSEL, len, len);
            std::wstring line = *s + L"\r\n";
            SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
            if (s->rfind(L"ERROR", 0) == 0) g_failed = true;
            SetWindowTextW(h_stepText, s->c_str());
            delete s;
            return 0;
        }
        case WM_PROG: SendMessageW(h_pbar, PBM_SETPOS, wp, 0); return 0;
        case WM_DONE:
            g_busy = false; g_failed = wp == 0;
            showPage(P_FINISH);
            if (st.silent) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE: if (g_busy) return 0; DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT mkFont(int px, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g_es = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    std::wstring cl = cmdLine ? cmdLine : L"";
    auto has = [&](const wchar_t* a) { return cl.find(a) != std::wstring::npos; };
    st.uninstall = has(L"--uninstall");
    st.silent = has(L"--silent");
    size_t g = cl.find(L"--gd");
    if (g != std::wstring::npos) {
        size_t a = cl.find(L'"', g), b = a == std::wstring::npos ? a : cl.find(L'"', a + 1);
        if (b != std::wstring::npos) st.gdDir = cl.substr(a + 1, b - a - 1);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (st.uninstall && st.gdDir.empty()) st.gdDir = regString(HKEY_CURRENT_USER, kOwnKey, L"GdDir");
    if (st.gdDir.empty()) st.gdDir = findGdFolder();
    if (st.uninstall && st.gdDir.empty()) { MessageBoxW(nullptr, T(L"No encuentro la carpeta del juego; solo se quitará lo demás.", L"Could not find the game folder; only the rest will be removed."), L"Kuang", MB_ICONINFORMATION); }

    UINT dpi = 96;
    if (auto fn = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")) dpi = fn();
    g_scale = dpi / 96.0;
    g_bodyBrush = CreateSolidBrush(kBodyBg);
    g_font = mkFont(14, FW_NORMAL); g_fontBig = mkFont(15, FW_SEMIBOLD); g_fontTitle = mkFont(34, FW_BOLD); g_fontSmall = mkFont(12, FW_NORMAL);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"KuangSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    int cw = S(680), ch = S(440);
    RECT r{0, 0, cw, ch};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, st.uninstall ? L"Kuang - Uninstall" : L"Kuang Setup", style,
        (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);

    h_pageTitle = mk(L"STATIC", L"", SS_LEFT, 224, 28, 430, 34, 0, g_fontTitle);
    SendMessageW(h_pageTitle, WM_SETFONT, (WPARAM)mkFont(24, FW_SEMIBOLD), TRUE);
    h_pageText = mk(L"STATIC", L"", SS_LEFT, 224, 80, 430, 260);
    h_path = mk(L"EDIT", st.gdDir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 224, 150, 340, 26, ID_PATH, nullptr, WS_EX_CLIENTEDGE);
    h_browse = mk(L"BUTTON", T(L"Examinar...", L"Browse..."), BS_PUSHBUTTON, 572, 149, 90, 28, ID_BROWSE);
    h_pathStatus = mk(L"STATIC", L"", SS_LEFT, 224, 186, 430, 24);
    for (int i = 0; i < kNumComps; i++) {
        h_comp[i] = mk(L"BUTTON", g_es ? kComps[i].nameEs : kComps[i].nameEn, BS_AUTOCHECKBOX, 224, 84 + i * 58, 430, 24, ID_COMP0 + i, g_fontBig);
        SendMessageW(h_comp[i], BM_SETCHECK, BST_CHECKED, 0);
        h_compDesc[i] = mk(L"STATIC", g_es ? kComps[i].descEs : kComps[i].descEn, SS_LEFT, 246, 108 + i * 58, 410, 22);
    }
    h_desk = mk(L"BUTTON", T(L"Acceso directo en el escritorio", L"Desktop shortcuts"), BS_AUTOCHECKBOX, 224, 268, 430, 24, ID_DESK);
    h_start = mk(L"BUTTON", T(L"Accesos directos en el menú Inicio (incluye Desinstalar)", L"Start menu shortcuts (includes Uninstall)"), BS_AUTOCHECKBOX, 224, 296, 430, 24, ID_START);
    SendMessageW(h_desk, BM_SETCHECK, BST_CHECKED, 0); SendMessageW(h_start, BM_SETCHECK, BST_CHECKED, 0);
    h_stepText = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 224, 84, 430, 22);
    h_pbar = mk(L"msctls_progress32", L"", PBS_SMOOTH, 224, 112, 430, 18, ID_PBAR);
    SendMessageW(h_pbar, PBM_SETRANGE32, 0, 100);
    h_log = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 224, 144, 430, 200, ID_LOG, nullptr, WS_EX_CLIENTEDGE);
    h_finishText = mk(L"STATIC", L"", SS_LEFT, 224, 76, 430, 62);
    h_launch = mk(L"BUTTON", T(L"Abrir Geometry Dash ahora", L"Launch Geometry Dash now"), BS_AUTOCHECKBOX, 224, 360, 430, 24, ID_LAUNCH);
    // finish page: log is shown shorter, under the text
    h_back = mk(L"BUTTON", T(L"< Atrás", L"< Back"), BS_PUSHBUTTON | WS_VISIBLE, 392, 398, 88, 30, ID_BACK);
    h_next = mk(L"BUTTON", T(L"Siguiente >", L"Next >"), BS_DEFPUSHBUTTON | WS_VISIBLE, 486, 398, 92, 30, ID_NEXT);
    h_cancel = mk(L"BUTTON", T(L"Cancelar", L"Cancel"), BS_PUSHBUTTON | WS_VISIBLE, 584, 398, 88, 30, ID_CANCEL);

    ShowWindow(g_hwnd, st.silent ? SW_HIDE : SW_SHOW);
    UpdateWindow(g_hwnd);
    if (st.silent) {
        if (!st.uninstall && !isGdFolder(st.gdDir)) return 2;
        SetWindowTextW(h_path, st.gdDir.c_str());
        showPage(P_PROGRESS);
    } else {
        showPage(P_WELCOME);
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return g_failed ? 1 : 0;
}
