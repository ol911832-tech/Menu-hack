// Kuang Optimizer - in-game part (kopt.dll), loaded by the winmm.dll proxy.
// Only things that have to happen inside Geometry Dash live here:
//   - flush the GPU queue after every frame (glFinish) and force V-Sync off
//   - CPU / GPU / I/O priority, cores, thread boost, 0.5 ms timer, power plan
//   - a low priority watchdog that puts them back if something else changes them
// Settings come from kopt.ini in the game folder and are reloaded live.
// Nothing here touches physics, hitboxes or inputs.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <powrprof.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "kopt_shared.h"

namespace {

std::wstring g_dir;
FILE* g_log = nullptr;

void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_log, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// ---------------------------------------------------------------------------
// Settings (kopt.ini)
// ---------------------------------------------------------------------------
struct Settings {
    bool enabled = true;
    bool flushGpu = false, noVsync = false, highPriority = false, watchdog = true;
    bool gpuPriority = false, ioPriority = false, affinity = false, kbdCore = false;
    bool noBoost = false, timer = false;
    std::wstring powerPlan; // GUID text, empty = don't change
};
Settings g_set;
SRWLOCK g_setLock = SRWLOCK_INIT;
FILETIME g_iniTime{};

std::wstring iniPath() { return g_dir + KOPT_INI_NAME; }

bool loadIniIfChanged(bool force) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(iniPath().c_str(), GetFileExInfoStandard, &fa)) return false;
    if (!force && CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return false;
    g_iniTime = fa.ftLastWriteTime;
    auto p = iniPath();
    auto geti = [&](const wchar_t* k, int d) { return GetPrivateProfileIntW(L"General", k, d, p.c_str()) != 0; };
    Settings s;
    s.enabled = geti(L"enabled", 1);
    s.flushGpu = geti(L"flush_gpu", 0);
    s.noVsync = geti(L"no_vsync", 0);
    s.highPriority = geti(L"high_priority", 0);
    s.watchdog = geti(L"watchdog", 1);
    s.gpuPriority = geti(L"gpu_priority", 0);
    s.ioPriority = geti(L"io_priority", 0);
    s.affinity = geti(L"affinity", 0);
    s.kbdCore = geti(L"kbd_core", 0);
    s.noBoost = geti(L"no_boost", 0);
    s.timer = geti(L"timer", 0);
    wchar_t plan[64] = {};
    GetPrivateProfileStringW(L"General", L"power_plan", L"", plan, 64, p.c_str());
    s.powerPlan = plan;
    AcquireSRWLockExclusive(&g_setLock);
    g_set = s;
    ReleaseSRWLockExclusive(&g_setLock);
    logf("settings: enabled=%d flush=%d novsync=%d prio=%d watchdog=%d gpu=%d io=%d aff=%d kbdcore=%d noboost=%d timer=%d plan=%ls",
        s.enabled, s.flushGpu, s.noVsync, s.highPriority, s.watchdog, s.gpuPriority, s.ioPriority, s.affinity,
        s.kbdCore, s.noBoost, s.timer, s.powerPlan.c_str());
    return true;
}

Settings settings() {
    AcquireSRWLockShared(&g_setLock);
    Settings s = g_set;
    ReleaseSRWLockShared(&g_setLock);
    return s;
}

// ---------------------------------------------------------------------------
// Status for the app
// ---------------------------------------------------------------------------
KoptStatus* g_status = nullptr;

void openStatus() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(KoptStatus), KOPT_STATUS_MAPPING);
    if (!h) return;
    g_status = static_cast<KoptStatus*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KoptStatus)));
    if (!g_status) return;
    std::memset(g_status, 0, sizeof(KoptStatus));
    g_status->magic = KOPT_STATUS_MAGIC;
    g_status->version = 1;
    g_status->pid = GetCurrentProcessId();
    g_status->state = KOPT_STATE_STARTING;
    g_status->gpuClass = -1;
    g_status->ioLevel = -1;
    g_status->heartbeat = GetTickCount64();
    std::snprintf(g_status->message, sizeof(g_status->message), "Loaded, waiting for the main menu...");
}

void setMessage(uint32_t state, const char* msg) {
    logf("%s", msg);
    if (!g_status) return;
    g_status->state = state;
    std::snprintf(g_status->message, sizeof(g_status->message), "%s", msg);
}

// ---------------------------------------------------------------------------
// Process tweaks (same logic as GD Optimizer)
// ---------------------------------------------------------------------------
using NtTimerSet = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
using NtTimerQuery = LONG(NTAPI*)(PULONG, PULONG, PULONG);
using KmtSet = LONG(WINAPI*)(HANDLE, int);
using KmtGet = LONG(WINAPI*)(HANDLE, int*);
using NtSetInfo = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG);
using NtQueryInfo = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
constexpr int kProcessIoPriority = 33;

template <class T>
T proc(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

int maskBits(DWORD_PTR m) { int n = 0; for (; m; m &= m - 1) n++; return n; }

// one logical CPU per physical core, skipping physical core 0; only the fast cores on hybrid CPUs
DWORD_PTR wantedAffinity(DWORD_PTR systemMask) {
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

// all threads of the last fast physical core (the one reserved for the keyboard)
DWORD_PTR reservedCore(DWORD_PTR systemMask) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return 0;
    std::vector<char> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) return 0;
    int best = -1;
    DWORD_PTR full = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (DWORD off = 0; off < len;) {
            auto cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (cur->Size == 0) break;
            if (cur->Relationship == RelationProcessorCore && cur->Processor.GroupCount > 0 && cur->Processor.GroupMask[0].Group == 0) {
                DWORD_PTR core = cur->Processor.GroupMask[0].Mask & systemMask;
                int cls = cur->Processor.EfficiencyClass;
                if (core && !(core & 1)) {
                    if (pass == 0) best = std::max(best, cls);
                    else if (cls == best && core > full) full = core;
                }
            }
            off += cur->Size;
        }
    }
    return full;
}

DWORD_PTR gameAffinity(DWORD_PTR systemMask, bool noSmt, bool kbdCore) {
    DWORD_PTR base = noSmt ? wantedAffinity(systemMask) : systemMask;
    if (!base) base = systemMask;
    if (kbdCore) {
        DWORD_PTR full = reservedCore(systemMask);
        if (full && maskBits(base & ~full) >= 2) base &= ~full;
    }
    return base;
}

struct Applied {
    bool priority = false, timer = false, gpu = false, boost = false, io = false;
    int affMode = 0;
    std::wstring plan;
    bool planSaved = false;
    GUID planBefore{};
} g_applied;
uint32_t g_corrections = 0;

bool parseGuid(const std::wstring& s, GUID& g) {
    if (s.empty()) return false;
    unsigned long d1; unsigned d2, d3, b[8];
    if (swscanf_s(s.c_str(), L"%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    g.Data1 = d1; g.Data2 = static_cast<unsigned short>(d2); g.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; i++) g.Data4[i] = static_cast<unsigned char>(b[i]);
    return true;
}

void restorePowerPlan() {
    if (g_applied.planSaved) {
        PowerSetActiveScheme(nullptr, &g_applied.planBefore);
        g_applied.planSaved = false;
        logf("power plan restored");
    }
    g_applied.plan.clear();
}

// apply what changed (on = setting, off = back to normal)
void applyProcess(const Settings& s, bool enforce) {
    HANDLE h = GetCurrentProcess();
    bool on = s.enabled;
    auto fixed = [&](const char* what) { if (enforce) { g_corrections++; logf("watchdog: fixed %s", what); } };

    bool want = on && s.highPriority;
    if (want != g_applied.priority) {
        SetPriorityClass(h, want ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
        g_applied.priority = want;
    } else if (want && enforce && GetPriorityClass(h) != HIGH_PRIORITY_CLASS) {
        SetPriorityClass(h, HIGH_PRIORITY_CLASS);
        fixed("CPU priority");
    }

    want = on && s.timer;
    if (want != g_applied.timer) {
        if (auto set = proc<NtTimerSet>(L"ntdll.dll", "NtSetTimerResolution")) {
            ULONG cur = 0;
            set(5000, want ? TRUE : FALSE, &cur); // 100 ns units: 5000 = 0.5 ms
        }
        g_applied.timer = want;
    } else if (want && enforce) {
        auto query = proc<NtTimerQuery>(L"ntdll.dll", "NtQueryTimerResolution");
        auto set = proc<NtTimerSet>(L"ntdll.dll", "NtSetTimerResolution");
        ULONG coarsest = 0, finest = 0, cur = 0;
        if (query && set && query(&coarsest, &finest, &cur) == 0 && cur > 5000 && finest <= 5000) {
            ULONG out = 0;
            set(5000, TRUE, &out);
            fixed("timer");
        }
    }

    want = on && s.gpuPriority;
    auto kget = proc<KmtGet>(L"gdi32.dll", "D3DKMTGetProcessSchedulingPriorityClass");
    auto kset = proc<KmtSet>(L"gdi32.dll", "D3DKMTSetProcessSchedulingPriorityClass");
    if (kset && want != g_applied.gpu) {
        kset(h, want ? 4 : 2);
        g_applied.gpu = want;
    } else if (want && enforce && kget && kset) {
        int cls = -1;
        if (kget(h, &cls) == 0 && cls != 4 && kset(h, 4) == 0) fixed("GPU priority");
    }

    int affMode = on ? (s.affinity ? 1 : 0) | (s.kbdCore ? 2 : 0) : 0;
    DWORD_PTR pm = 0, sm = 0;
    if (GetProcessAffinityMask(h, &pm, &sm)) {
        if (affMode != g_applied.affMode) {
            SetProcessAffinityMask(h, affMode ? gameAffinity(sm, affMode & 1, affMode & 2) : sm);
            g_applied.affMode = affMode;
        } else if (affMode && enforce) {
            DWORD_PTR target = gameAffinity(sm, affMode & 1, affMode & 2);
            if (target && pm != target && SetProcessAffinityMask(h, target)) fixed("cores");
        }
    }

    want = on && s.noBoost;
    BOOL disabled = FALSE;
    if (want != g_applied.boost) {
        SetProcessPriorityBoost(h, want ? TRUE : FALSE);
        g_applied.boost = want;
    } else if (want && enforce && GetProcessPriorityBoost(h, &disabled) && !disabled) {
        SetProcessPriorityBoost(h, TRUE);
        fixed("thread boost");
    }

    // high I/O priority usually needs administrator; the app (elevated) also enforces it from outside
    want = on && s.ioPriority;
    auto nset = proc<NtSetInfo>(L"ntdll.dll", "NtSetInformationProcess");
    if (nset && want != g_applied.io) {
        ULONG level = want ? 3 : 2;
        nset(h, kProcessIoPriority, &level, sizeof(level));
        g_applied.io = want;
    }

    std::wstring plan = on ? s.powerPlan : std::wstring();
    GUID g{};
    if (plan != g_applied.plan) {
        if (plan.empty() || !parseGuid(plan, g)) {
            restorePowerPlan();
        } else {
            if (!g_applied.planSaved) {
                GUID* cur = nullptr;
                if (PowerGetActiveScheme(nullptr, &cur) == ERROR_SUCCESS && cur) {
                    g_applied.planBefore = *cur;
                    g_applied.planSaved = true;
                    LocalFree(cur);
                }
            }
            PowerSetActiveScheme(nullptr, &g);
            g_applied.plan = plan;
        }
    } else if (!plan.empty() && enforce && parseGuid(plan, g)) {
        GUID* cur = nullptr;
        if (PowerGetActiveScheme(nullptr, &cur) == ERROR_SUCCESS && cur) {
            if (!IsEqualGUID(*cur, g) && PowerSetActiveScheme(nullptr, &g) == ERROR_SUCCESS) fixed("power plan");
            LocalFree(cur);
        }
    }
}

void publishProcessState() {
    if (!g_status) return;
    HANDLE h = GetCurrentProcess();
    g_status->heartbeat = GetTickCount64();
    Settings s = settings();
    g_status->enabled = s.enabled;
    g_status->priorityClass = GetPriorityClass(h);
    DWORD_PTR pm = 0, sm = 0;
    if (GetProcessAffinityMask(h, &pm, &sm)) { g_status->affinity = pm; g_status->systemAffinity = sm; }
    if (auto query = proc<NtTimerQuery>(L"ntdll.dll", "NtQueryTimerResolution")) {
        ULONG a = 0, b = 0, c = 0;
        if (query(&a, &b, &c) == 0) g_status->timerRes = c;
    }
    int cls = -1;
    if (auto kget = proc<KmtGet>(L"gdi32.dll", "D3DKMTGetProcessSchedulingPriorityClass")) kget(h, &cls);
    g_status->gpuClass = cls;
    ULONG io = 0, got = 0;
    auto nq = proc<NtQueryInfo>(L"ntdll.dll", "NtQueryInformationProcess");
    g_status->ioLevel = (nq && nq(h, kProcessIoPriority, &io, sizeof(io), &got) == 0) ? static_cast<int32_t>(io) : -1;
    BOOL dis = FALSE;
    GetProcessPriorityBoost(h, &dis);
    g_status->boostDisabled = dis;
    g_status->corrections = g_corrections;
    GUID* cur = nullptr;
    g_status->plan[0] = 0;
    if (PowerGetActiveScheme(nullptr, &cur) == ERROR_SUCCESS && cur) {
        wchar_t name[96] = {};
        DWORD size = sizeof(name);
        if (PowerReadFriendlyName(nullptr, cur, nullptr, nullptr, reinterpret_cast<UCHAR*>(name), &size) == ERROR_SUCCESS)
            WideCharToMultiByte(CP_UTF8, 0, name, -1, g_status->plan, sizeof(g_status->plan) - 1, nullptr, nullptr);
        LocalFree(cur);
    }
}

// low priority watchdog: reload settings, re-apply, publish status
DWORD WINAPI watchThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    int tick = 0;
    while (true) {
        SetThreadAffinityMask(GetCurrentThread(), 1); // core 0 when allowed
        bool changed = loadIniIfChanged(false);
        Settings s = settings();
        applyProcess(s, !changed && s.watchdog && (tick % 4 == 0)); // enforce every 2 s
        publishProcessState();
        tick++;
        Sleep(500);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Render hook: flush GPU queue, V-Sync off
// ---------------------------------------------------------------------------
using FnV = void (*)(void*);
FnV o_drawScene = nullptr;
using GlFinish = void(WINAPI*)();
using SwapIntervalExt = BOOL(WINAPI*)(int);
using WglGetProc = PROC(WINAPI*)(LPCSTR);
GlFinish g_glFinish = nullptr;
SwapIntervalExt g_swapInterval = nullptr;
bool g_vsyncDone = false;
ULONGLONG g_lastVsync = 0;
uint64_t g_frames = 0;
ULONGLONG g_fpsStart = 0;

void h_drawScene(void* self) {
    o_drawScene(self);
    Settings s = settings();
    bool flush = s.enabled && s.flushGpu;
    if (flush && g_glFinish) g_glFinish(); // wait for the GPU: no frames piling up in a queue
    if (s.enabled && s.noVsync) {
        ULONGLONG now = GetTickCount64();
        if (!g_swapInterval) {
            if (auto wglGet = proc<WglGetProc>(L"opengl32.dll", "wglGetProcAddress"))
                g_swapInterval = reinterpret_cast<SwapIntervalExt>(wglGet("wglSwapIntervalEXT"));
        }
        // GD can turn it back on (options, entering a level): set it again every 2 s
        if (g_swapInterval && (!g_vsyncDone || now - g_lastVsync > 2000)) {
            g_swapInterval(0);
            g_vsyncDone = true;
            g_lastVsync = now;
        }
    } else {
        g_vsyncDone = false;
    }
    if (g_status) {
        g_status->flushGpu = flush;
        g_status->vsyncOff = s.enabled && s.noVsync && g_vsyncDone;
        g_frames++;
        ULONGLONG now = GetTickCount64();
        if (!g_fpsStart) g_fpsStart = now;
        if (now - g_fpsStart >= 1000) {
            g_status->fps = static_cast<float>(g_frames * 1000.0 / (now - g_fpsStart));
            g_frames = 0;
            g_fpsStart = now;
        }
    }
}

// main menu reached = game and Geode (if any) finished hooking
const char* rttiName(void* obj) {
    auto vt = *reinterpret_cast<void***>(obj);
    auto col = reinterpret_cast<const uint32_t*>(vt[-1]);
    if (!col || col[0] != 1) return nullptr;
    uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];
    return reinterpret_cast<const char*>(base + col[3] + 16);
}

bool atMenuSEH(void* (*sharedDirector)(), void* (*runningScene)(void*)) {
    __try {
        void* dir = sharedDirector();
        if (!dir) return false;
        void* scene = runningScene(dir);
        if (!scene) return false;
        void* ch = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(scene) + 0xd0);
        if (!ch) return false;
        void* data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ch) + 0x38);
        if (!data) return false;
        unsigned c = *reinterpret_cast<unsigned*>(data);
        void** arr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(data) + 0x10);
        for (unsigned i = 0; i < c && i < 16; i++) {
            const char* n = rttiName(arr[i]);
            if (n && (std::strcmp(n, ".?AVMenuLayer@@") == 0 || std::strcmp(n, ".?AVPlayLayer@@") == 0 ||
                      std::strcmp(n, ".?AVLevelSelectLayer@@") == 0))
                return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

DWORD WINAPI initThread(LPVOID) {
    // process tweaks right away (they don't touch the game's code)
    loadIniIfChanged(true);
    applyProcess(settings(), false);
    if (HANDLE t = CreateThread(nullptr, 0, watchThread, nullptr, 0, nullptr)) CloseHandle(t);

    for (int i = 0; i < 600 && !GetModuleHandleW(L"libcocos2d.dll"); i++) Sleep(100);
    HMODULE cc = GetModuleHandleW(L"libcocos2d.dll");
    if (!cc) { setMessage(KOPT_STATE_ERROR, "libcocos2d.dll not found. GPU options disabled."); return 0; }
    auto sharedDirector = reinterpret_cast<void* (*)()>(GetProcAddress(cc, "?sharedDirector@CCDirector@cocos2d@@SAPEAV12@XZ"));
    auto runningScene = reinterpret_cast<void* (*)(void*)>(GetProcAddress(cc, "?getRunningScene@CCDirector@cocos2d@@QEAAPEAVCCScene@2@XZ"));
    void* drawScene = reinterpret_cast<void*>(GetProcAddress(cc, "?drawScene@CCDirector@cocos2d@@QEAAXXZ"));
    g_glFinish = proc<GlFinish>(L"opengl32.dll", "glFinish");
    if (!sharedDirector || !runningScene || !drawScene) {
        setMessage(KOPT_STATE_ERROR, "cocos2d functions not found. GPU options disabled.");
        return 0;
    }
    for (int i = 0; i < 3000 && !atMenuSEH(sharedDirector, runningScene); i++) Sleep(100);
    Sleep(500);
    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(drawScene, reinterpret_cast<void*>(&h_drawScene), reinterpret_cast<void**>(&o_drawScene)) != MH_OK ||
        MH_EnableHook(drawScene) != MH_OK) {
        setMessage(KOPT_STATE_ERROR, "Could not hook the renderer. GPU options disabled; the rest works.");
        return 0;
    }
    setMessage(KOPT_STATE_ACTIVE, "Active.");
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        std::wstring p = path;
        g_dir = p.substr(0, p.find_last_of(L"\\/") + 1);
        g_log = _wfopen((g_dir + KOPT_LOG_NAME).c_str(), L"w");
        logf("Kuang Optimizer %s loaded", KOPT_VERSION_STR);
        openStatus();
        if (HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        // the power plan belongs to all of Windows: give back the one that was active
        restorePowerPlan();
        (void)reserved;
    }
    return TRUE;
}
