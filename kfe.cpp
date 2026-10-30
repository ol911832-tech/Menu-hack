// Kuang Frame Extrapolation - standalone mod for Geometry Dash 2.2091 (Windows).
// No Geode needed. Loaded by the winmm.dll proxy, hooks with MinHook,
// settings in kfe.ini (reloaded live), status shared with the app.
//
// How it works:
//  GD simulates physics in fixed 1/240 s ticks and keeps the leftover time in
//  GJBaseGameLayer::m_extraDelta. What is drawn is therefore slightly ahead or
//  behind real time. After every physics tick we record camera, icons, background
//  and ground; right before drawing we move them to where they would be at real
//  time, and after drawing we put them back. Physics never sees the moved values.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "../shared/kfe_shared.h"

namespace {

// ---------------------------------------------------------------------------
// GD addresses / offsets for GeometryDash.exe 2.2091 (re-found by byte signature + disassembly of the
// new exe on 2026-10-24). Compared with the 2026-10-20 build, GJBaseGameLayer lost those 0x40 bytes
// again: m_player1 is back at 0xdb8 and every field from there up moved -0x40 (objectLayer 0x10c8,
// extraDelta 0x3350 ...). timeWarp (0x330), PlayerObject, GameObject and CCNode fields did not move.
// ---------------------------------------------------------------------------
namespace gd {
    constexpr uintptr_t updateCamera = 0x345c60; // GJBaseGameLayer::updateCamera(float)
    constexpr uintptr_t update       = 0x3406d0; // GJBaseGameLayer::update(float)
    // GJBaseGameLayer::updateObjectSection(GameObject*): GD calls it for every object that a
    // Move / Rotate / Follow / Area trigger moves, so it tells us which objects are moving.
    constexpr uintptr_t updateObjectSection = 0x330ba0;
    constexpr unsigned char updateObjectSectionSig[] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0x48,
        0x8b, 0xda, 0x8b, 0x8a, 0x78, 0x02, 0x00, 0x00, 0x85, 0xc9,
    };
    constexpr unsigned char updateCameraSig[] = {
        0x48, 0x8b, 0xc4, 0xf3, 0x0f, 0x11, 0x48, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54,
        0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xe8, 0xfe, 0xff, 0xff, 0x48,
        0x81, 0xec, 0xd8, 0x01, 0x00, 0x00,
    };
    constexpr unsigned char updateSig[] = {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0x90, 0x48, 0x81, 0xec, 0x70, 0x01,
        0x00, 0x00,
    };
    constexpr size_t timeWarp         = 0x330;
    constexpr size_t player1          = 0xdb8;
    constexpr size_t player2          = 0xdc0;
    constexpr size_t objectLayer      = 0x10c8;
    constexpr size_t inShaderLayer    = 0x10d0;
    constexpr size_t aboveShaderLayer = 0x10d8;
    constexpr size_t background       = 0x10e0;
    constexpr size_t groundLayer      = 0x10f0;
    constexpr size_t groundLayer2     = 0x10f8;
    constexpr size_t middleground     = 0x1100;
    constexpr size_t extraDelta       = 0x3350;
    // PlayerObject trails (toggleDartMode / PlayerObject::setPosition)
    constexpr size_t regularTrail     = 0x7a8; // CCMotionStreak*
    constexpr size_t shipStreak       = 0x7b0; // CCMotionStreak* (ship / UFO fire)
    constexpr size_t waveTrail        = 0x7b8; // HardStreak*
    // HardStreak (GD's wave trail, a CCDrawNode). The game rebuilds it every frame from the scheduler with
    // updateStroke: stored corners + m_currentPoint as the head (all relative to the node position).
    constexpr uintptr_t hardStreakUpdateStroke = 0x3bfb60; // HardStreak::updateStroke(float)
    constexpr unsigned char hardStreakUpdateStrokeSig[] = {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    };
    constexpr size_t hsPointArray     = 0x1a0; // CCArray* of stored corners
    constexpr size_t hsCurrentPoint   = 0x1a8; // CCPoint: the head of the wave trail (the player position)
    constexpr size_t hsDrawStreak     = 0x1ba; // bool
    // GameObject: real (physics) position, doubles
    constexpr size_t objPositionX     = 0x3b8;
    constexpr size_t objPositionY     = 0x3c0;
}

// cocos2d::CCMotionStreak (GD's libcocos2d, from CCMotionStreak::update / draw / setPosition / stopStroke)
namespace streak {
    constexpr size_t started   = 0x161; // m_bStartingPositionInitialized
    constexpr size_t stroking  = 0x162; // false after stopStroke
    constexpr size_t width     = 0x178; // m_fStroke
    constexpr size_t maxPoints = 0x184;
    constexpr size_t nuPoints  = 0x188;
    constexpr size_t points    = 0x190; // CCPoint*
    constexpr size_t vertices  = 0x1a0; // ccVertex2F*, 2 per point
    constexpr size_t colors    = 0x1a8; // 4 bytes per vertex
    constexpr size_t texCoords = 0x1b0; // ccTex2F, 2 per point
    constexpr size_t maxSeg    = 0x1d0; // GD: a jump longer than this restarts the trail
    constexpr size_t positionR = 0x1d8; // where the game wants the trail head (set every physics tick)
    // Points and vertices are LOCAL to the streak node: the game moves the node to positionR and shifts the
    // stored points back, so the newest point is at (positionR - node position).
}

// cocos2d::CCNode fields (same layout in 2.2081, 2.2091 and the 2026-10-20 patch)
namespace ccnode {
    constexpr size_t rotationX = 0x38;
    constexpr size_t scaleX    = 0x40;
    constexpr size_t scaleY    = 0x44;
    constexpr size_t position  = 0x4c;
    constexpr size_t children  = 0xd0;
    constexpr size_t parent    = 0xd8;
    constexpr size_t visible   = 0x11e;
}

template <class T>
T& field(void* obj, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(obj) + off);
}

// ---------------------------------------------------------------------------
// Paths, log, ini
// ---------------------------------------------------------------------------
std::wstring g_dir; // folder of kfe.dll (= GD folder)
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

struct Settings {
    bool enabled = true;
    bool interpolate = false;
    bool smartPredict = true;
    bool background = true;
    bool rotation = true;
    bool objects = true;
    bool trails = true;
    bool debug = false;
} g_set;

FILETIME g_iniTime{};
ULONGLONG g_lastIniCheck = 0;

std::wstring iniPath() { return g_dir + KFE_INI_NAME; }

void writeDefaultIni() {
    auto p = iniPath();
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    WritePrivateProfileStringW(L"General", L"enabled", L"1", p.c_str());
    WritePrivateProfileStringW(L"General", L"mode", L"extrapolation", p.c_str());
    WritePrivateProfileStringW(L"General", L"smart_prediction", L"1", p.c_str());
    WritePrivateProfileStringW(L"General", L"smooth_background", L"1", p.c_str());
    WritePrivateProfileStringW(L"General", L"smooth_rotation", L"1", p.c_str());
    WritePrivateProfileStringW(L"General", L"smooth_objects", L"1", p.c_str());
    WritePrivateProfileStringW(L"General", L"smooth_trails", L"1", p.c_str());
    WritePrivateProfileStringW(L"General", L"debug_log", L"0", p.c_str());
}

void loadIni() {
    auto p = iniPath();
    auto geti = [&](const wchar_t* k, int def) { return GetPrivateProfileIntW(L"General", k, def, p.c_str()) != 0; };
    g_set.enabled = geti(L"enabled", 1);
    g_set.smartPredict = geti(L"smart_prediction", 1);
    g_set.background = geti(L"smooth_background", 1);
    g_set.rotation = geti(L"smooth_rotation", 1);
    g_set.objects = geti(L"smooth_objects", 1);
    g_set.trails = geti(L"smooth_trails", 1);
    g_set.debug = geti(L"debug_log", 0);
    wchar_t mode[64] = {};
    GetPrivateProfileStringW(L"General", L"mode", L"extrapolation", mode, 64, p.c_str());
    g_set.interpolate = _wcsicmp(mode, L"interpolation") == 0;
    logf("settings: enabled=%d mode=%s smart=%d bg=%d rot=%d objects=%d trails=%d debug=%d", g_set.enabled,
        g_set.interpolate ? "interpolation" : "extrapolation", g_set.smartPredict, g_set.background,
        g_set.rotation, g_set.objects, g_set.trails, g_set.debug);
}

// called from the main thread, cheap: only looks at the file time twice per second
void maybeReloadIni() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastIniCheck < 500) return;
    g_lastIniCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(iniPath().c_str(), GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) != 0) {
        g_iniTime = fa.ftLastWriteTime;
        loadIni();
    }
}

// ---------------------------------------------------------------------------
// Status shared with the app
// ---------------------------------------------------------------------------
KfeStatus* g_status = nullptr;

void openStatus() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(KfeStatus), KFE_STATUS_MAPPING);
    if (!h) return;
    g_status = static_cast<KfeStatus*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KfeStatus)));
    if (!g_status) return;
    std::memset(g_status, 0, sizeof(KfeStatus));
    g_status->magic = KFE_STATUS_MAGIC;
    g_status->version = 1;
    g_status->pid = GetCurrentProcessId();
    g_status->state = KFE_STATE_STARTING;
    g_status->heartbeat = GetTickCount64();
    std::snprintf(g_status->message, sizeof(g_status->message), "Loaded, waiting for the main menu...");
}

void setState(uint32_t st, const char* msg) {
    logf("%s", msg);
    if (!g_status) return;
    g_status->state = st;
    std::snprintf(g_status->message, sizeof(g_status->message), "%s", msg);
    g_status->heartbeat = GetTickCount64();
}

// ---------------------------------------------------------------------------
// cocos2d helpers (resolved at runtime from libcocos2d.dll)
// ---------------------------------------------------------------------------
struct Cocos {
    size_t setPosition = 0, setScaleX = 0, setScaleY = 0, setRotation = 0;
    void* (*sharedDirector)() = nullptr;
    void* (*getRunningScene)(void*) = nullptr;
    void (*retain)(void*) = nullptr;
    void (*release)(void*) = nullptr;
    void* schedulerUpdate = nullptr;
    void* drawScene = nullptr;
} cc;

bool resolveCocos() {
    HMODULE m = GetModuleHandleW(L"libcocos2d.dll");
    if (!m) return false;
    auto vt = reinterpret_cast<void**>(GetProcAddress(m, "??_7CCNode@cocos2d@@6B@"));
    void* fnPos = reinterpret_cast<void*>(GetProcAddress(m, "?setPosition@CCNode@cocos2d@@UEAAXAEBVCCPoint@2@@Z"));
    void* fnSX = reinterpret_cast<void*>(GetProcAddress(m, "?setScaleX@CCNode@cocos2d@@UEAAXM@Z"));
    void* fnSY = reinterpret_cast<void*>(GetProcAddress(m, "?setScaleY@CCNode@cocos2d@@UEAAXM@Z"));
    void* fnRot = reinterpret_cast<void*>(GetProcAddress(m, "?setRotation@CCNode@cocos2d@@UEAAXM@Z"));
    cc.sharedDirector = reinterpret_cast<void* (*)()>(GetProcAddress(m, "?sharedDirector@CCDirector@cocos2d@@SAPEAV12@XZ"));
    cc.getRunningScene = reinterpret_cast<void* (*)(void*)>(GetProcAddress(m, "?getRunningScene@CCDirector@cocos2d@@QEAAPEAVCCScene@2@XZ"));
    cc.retain = reinterpret_cast<void (*)(void*)>(GetProcAddress(m, "?retain@CCObject@cocos2d@@QEAAXXZ"));
    cc.release = reinterpret_cast<void (*)(void*)>(GetProcAddress(m, "?release@CCObject@cocos2d@@QEAAXXZ"));
    cc.schedulerUpdate = reinterpret_cast<void*>(GetProcAddress(m, "?update@CCScheduler@cocos2d@@UEAAXM@Z"));
    cc.drawScene = reinterpret_cast<void*>(GetProcAddress(m, "?drawScene@CCDirector@cocos2d@@QEAAXXZ"));
    if (!vt || !fnPos || !fnSX || !fnSY || !fnRot || !cc.sharedDirector || !cc.getRunningScene || !cc.retain ||
        !cc.release || !cc.schedulerUpdate || !cc.drawScene)
        return false;
    int found = 0;
    for (size_t i = 0; i < 200; i++) {
        void* e = vt[i];
        if (e == fnPos && !cc.setPosition) { cc.setPosition = i; found++; }
        else if (e == fnSX && !cc.setScaleX) { cc.setScaleX = i; found++; }
        else if (e == fnSY && !cc.setScaleY) { cc.setScaleY = i; found++; }
        else if (e == fnRot && !cc.setRotation) { cc.setRotation = i; found++; }
    }
    logf("CCNode vtable: setPosition=%zu setScaleX=%zu setScaleY=%zu setRotation=%zu", cc.setPosition,
        cc.setScaleX, cc.setScaleY, cc.setRotation);
    return found == 4;
}

template <class... A>
void vcall(void* obj, size_t slot, A... args) {
    auto vt = *reinterpret_cast<void***>(obj);
    reinterpret_cast<void (*)(void*, A...)>(vt[slot])(obj, args...);
}

struct CCPoint { float x, y; };
void nodeSetPosition(void* n, float x, float y) { CCPoint p{x, y}; vcall<CCPoint const*>(n, cc.setPosition, &p); }
void nodeSetScaleX(void* n, float v) { vcall<float>(n, cc.setScaleX, v); }
void nodeSetScaleY(void* n, float v) { vcall<float>(n, cc.setScaleY, v); }
void nodeSetRotation(void* n, float v) { vcall<float>(n, cc.setRotation, v); }

unsigned arrCount(void* arr) {
    if (!arr) return 0;
    void* data = field<void*>(arr, 0x38);
    return data ? field<unsigned>(data, 0x0) : 0;
}
void* arrAt(void* arr, unsigned i) { return field<void**>(field<void*>(arr, 0x38), 0x10)[i]; }

// MSVC RTTI: ".?AVPlayLayer@@" etc. No headers needed.
const char* rttiName(void* obj) {
    auto vt = *reinterpret_cast<void***>(obj);
    auto col = reinterpret_cast<const uint32_t*>(vt[-1]);
    if (!col || col[0] != 1) return nullptr; // x64 COL signature
    uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5]; // pSelf rva
    return reinterpret_cast<const char*>(base + col[3] + 16);  // TypeDescriptor.name
}

bool isPlayLayer(void* obj) {
    const char* n = rttiName(obj);
    return n && std::strcmp(n, ".?AVPlayLayer@@") == 0;
}

// ---------------------------------------------------------------------------
// Tracking state
// ---------------------------------------------------------------------------
enum class Kind : uint8_t { Layer, Player, Plain, Object };

struct Vals { float x = 0, y = 0, sx = 1, sy = 1, rot = 0; };

Vals readVals(void* n) {
    Vals v;
    v.x = field<float>(n, ccnode::position);
    v.y = field<float>(n, ccnode::position + 4);
    v.sx = field<float>(n, ccnode::scaleX);
    v.sy = field<float>(n, ccnode::scaleY);
    v.rot = field<float>(n, ccnode::rotationX);
    return v;
}

// ---- camera space ("model") --------------------------------------------------------------
// The layers that hold the level (and the background / ground nodes) are placed by the camera as
// position = -worldOffset * scale. During a Zoom trigger the scale changes every tick, so the raw
// position changes by thousands of units * the zoom speed and its "acceleration" is huge and noisy,
// while the icon inside the layer is predicted in world units. Predicting the two in different
// spaces made the icon (and everything that follows the camera) shake against the level.
// So for these nodes we track and predict (worldOffset, scale) instead and rebuild the position
// from them: the camera is predicted in exactly the same space as the icon and the objects.
// With a constant scale this is the same as before (worldOffset is just -position / scale).
inline bool usesCameraSpace(Kind k) { return k == Kind::Layer || k == Kind::Plain; }
constexpr float kMinScale = 1e-3f;

Vals toModel(Kind k, Vals const& raw) {
    if (!usesCameraSpace(k) || std::fabs(raw.sx) < kMinScale || std::fabs(raw.sy) < kMinScale) return raw;
    Vals m = raw;
    m.x = -raw.x / raw.sx;
    m.y = -raw.y / raw.sy;
    return m;
}

Vals fromModel(Kind k, Vals const& m) {
    if (!usesCameraSpace(k) || std::fabs(m.sx) < kMinScale || std::fabs(m.sy) < kMinScale) return m;
    Vals r = m;
    r.x = -m.x * m.sx;
    r.y = -m.y * m.sy;
    return r;
}

struct Sample { double t = 0; Vals v; };

struct Track {
    Sample s[3];
    int n = 0;
    uint32_t seen = 0;
    Kind kind = Kind::Plain;
    float restoredRot = 0;
    bool hasRestoredRot = false;
    int actionRotFrames = 0;
};

struct Applied { void* node; Kind kind; Vals orig; Vals shown; };

// a GameObject moved by triggers. Position comes from its physics position (doubles),
// rotation from the sprite.
struct ObjTrack {
    Track tr;
    uint32_t lastMove = 0; // serial of the last tick in which GD moved it
};

// Tracked objects live in one contiguous vector (fast to walk every tick / every frame) plus a
// pointer -> index map used only when GD reports a moved object.
struct ObjEntry {
    void* obj;
    ObjTrack ot;
};

struct State {
    void* layer = nullptr;
    std::vector<ObjEntry> objList;                // objects are retained while tracked
    std::unordered_map<void*, uint32_t> objIndex;
    double t = 0;
    uint32_t serial = 0;
    std::unordered_map<void*, Track> tracks;
    std::vector<Applied> applied;
    std::vector<std::pair<void*, Kind>> nodes;
    bool hooked = false;
    // stats
    uint64_t frames = 0, samples = 0, moved = 0, movedObjs = 0, skippedObjs = 0;
    double hSum = 0;
    ULONGLONG statStart = 0, lastBeat = 0;
    uint64_t debugFrames = 0;
} g;

void addTree(std::vector<std::pair<void*, Kind>>& out, void* node, int depth) {
    if (!node) return;
    out.push_back({node, Kind::Plain});
    if (depth <= 0) return;
    void* ch = field<void*>(node, ccnode::children);
    unsigned c = arrCount(ch);
    if (c > 256) return;
    for (unsigned i = 0; i < c; i++) addTree(out, arrAt(ch, i), depth - 1);
}

void collectNodes(void* layer, std::vector<std::pair<void*, Kind>>& out) {
    out.clear();
    auto add = [&](size_t off, Kind k) {
        void* n = field<void*>(layer, off);
        if (n) out.push_back({n, k});
    };
    add(gd::objectLayer, Kind::Layer);
    add(gd::inShaderLayer, Kind::Layer);
    add(gd::aboveShaderLayer, Kind::Layer);
    add(gd::player1, Kind::Player);
    add(gd::player2, Kind::Player);
    if (g_set.background) {
        add(gd::background, Kind::Plain);
        addTree(out, field<void*>(layer, gd::groundLayer), 3);
        addTree(out, field<void*>(layer, gd::groundLayer2), 3);
        addTree(out, field<void*>(layer, gd::middleground), 3);
    }
}

bool isDiscontinuity(Kind k, Vals const& a, Vals const& b) {
    float th = (k == Kind::Player) ? 40.f : (k == Kind::Object ? 30.f : 60.f);
    if (std::fabs(a.x - b.x) > th || std::fabs(a.y - b.y) > th) return true;
    if (std::fabs(a.sx - b.sx) > 0.25f || std::fabs(a.sy - b.sy) > 0.25f) return true;
    if (k == Kind::Player || k == Kind::Object) {
        float d = std::fmod(std::fabs(a.rot - b.rot), 360.f);
        if (d > 180.f) d = 360.f - d;
        if (d > 100.f) return true;
    }
    return false;
}

bool nearlySame(Vals const& a, Vals const& b) {
    constexpr float e = 1e-4f;
    return std::fabs(a.x - b.x) < e && std::fabs(a.y - b.y) < e && std::fabs(a.sx - b.sx) < e &&
        std::fabs(a.sy - b.sy) < e;
}

void pushSample(Track& tr, Kind kind, Vals const& v, bool advance) {
    tr.kind = kind;
    tr.seen = g.serial;
    if (tr.n == 0) { tr.s[0] = {g.t, v}; tr.n = 1; return; }
    Sample& last = tr.s[tr.n - 1];
    bool disc = isDiscontinuity(kind, last.v, v);
    if (!advance) {
        if (disc) { tr.s[0] = {g.t, v}; tr.n = 1; }
        else last.v = v;
        return;
    }
    if (disc) { tr.s[0] = {g.t, v}; tr.n = 1; return; }
    if (tr.n == 3) { tr.s[0] = tr.s[1]; tr.s[1] = tr.s[2]; tr.n = 2; }
    tr.s[tr.n++] = {g.t, v};
}

// moving objects: physics position (doubles) + sprite rotation
Vals readObjVals(void* obj) {
    Vals v;
    v.x = static_cast<float>(field<double>(obj, gd::objPositionX));
    v.y = static_cast<float>(field<double>(obj, gd::objPositionY));
    v.rot = field<float>(obj, ccnode::rotationX);
    return v;
}

void clearObjs() {
    for (auto& e : g.objList) cc.release(e.obj);
    g.objList.clear();
    g.objIndex.clear();
}

// swap-and-pop removal (keeps the vector contiguous)
void objRemoveAt(size_t i) {
    ObjEntry& e = g.objList[i];
    cc.release(e.obj);
    g.objIndex.erase(e.obj);
    size_t last = g.objList.size() - 1;
    if (i != last) {
        e = g.objList[last];
        g.objIndex[e.obj] = static_cast<uint32_t>(i);
    }
    g.objList.pop_back();
}

// ---- cost control -------------------------------------------------------------------------
// Levels with thousands of moving objects can make the per-object smoothing more expensive than
// the stutter it removes. We measure the time spent in this mod every frame and, if it grows past a
// small share of the frame, smooth only 1 of every N objects (a fixed subset, chosen by address, so
// nothing flickers). When the cost drops again the subset grows back. Camera, icon and background
// smoothing are never touched by this.
double qpcMs() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / freq;
}
unsigned g_objStride = 1;      // 1 = every visible object, 2 = every second one, ...
double g_frameCostMs = 0;      // time spent in this mod since the last evaluation
double g_costEma = 0, g_lastFrameAt = 0, g_frameMsEma = 16.0;
int g_costFrames = 0;
constexpr size_t kMaxTracked = 6000;

inline bool objectWanted(void* obj) {
    if (g_objStride > 1 && ((reinterpret_cast<uintptr_t>(obj) >> 4) % g_objStride) != 0) return false;
    // hidden / detached objects are off screen: nothing to smooth
    return field<bool>(obj, ccnode::visible) && field<void*>(obj, ccnode::parent) != nullptr;
}

void updateCostControl() {
    double t = qpcMs();
    if (g_lastFrameAt > 0) {
        double ft = t - g_lastFrameAt;
        if (ft > 0.05 && ft < 250.0) g_frameMsEma = g_frameMsEma * 0.95 + ft * 0.05;
    }
    g_lastFrameAt = t;
    g_costEma = g_costEma * 0.9 + g_frameCostMs * 0.1;
    g_frameCostMs = 0;
    if (++g_costFrames < 45) return;
    g_costFrames = 0;
    double budget = std::clamp(g_frameMsEma * 0.15, 0.4, 3.0);
    unsigned old = g_objStride;
    if (g_costEma > budget && g_objStride < 32) g_objStride *= 2;
    else if (g_costEma < budget * 0.35 && g_objStride > 1) g_objStride /= 2;
    if (g_objStride != old)
        logf("[perf] object smoothing: 1 of every %u objects (cost %.2f ms/frame, budget %.2f ms, %zu tracked)",
            g_objStride, g_costEma, budget, g.objList.size());
}

// ---------------------------------------------------------------------------
// 1) sample after every physics tick
// ---------------------------------------------------------------------------
// Sanity check of the field offsets: if the game changes again, do nothing instead of crashing.
// 1 = ok, 0 = level still loading (players not created yet), -1 = the fields are not where we expect.
bool g_layoutOk = false, g_layoutBad = false;
int layoutCheckSEH(void* layer) {
    __try {
        void* p1 = field<void*>(layer, gd::player1);
        void* ol = field<void*>(layer, gd::objectLayer);
        if (!p1 || !ol) return 0;
        const char* n = rttiName(p1);
        if (!n || std::strcmp(n, ".?AVPlayerObject@@") != 0) return -1;
        return rttiName(ol) ? 1 : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void clearTrailKinds(); // trails section below

void onCameraSample(void* layer, float dt) {
    if (!g_set.enabled || g_layoutBad || !isPlayLayer(layer)) return;
    if (!g_layoutOk) {
        static int mismatches = 0;
        int r = layoutCheckSEH(layer);
        if (r == 0) return;
        if (r < 0) {
            if (++mismatches >= 30) {
                g_layoutBad = true;
                setState(KFE_STATE_WRONG_VERSION, "Field layout does not match this Geometry Dash build. Smoothing disabled.");
                logf("layout check failed: m_player1 / m_objectLayer are not where expected");
            }
            return;
        }
        g_layoutOk = true;
        logf("layout check ok");
    }
    if (layer != g.layer) {
        g.layer = layer;
        clearTrailKinds();
        g.tracks.clear();
        clearObjs();
        g.t = 0;
    }
    g.serial++;
    bool advance = dt > 1e-6f;
    if (advance) { g.t += dt; g.samples++; }

    collectNodes(layer, g.nodes);
    for (auto& [node, kind] : g.nodes) pushSample(g.tracks[node], kind, toModel(kind, readVals(node)), advance);

    // objects moved by triggers (collected by the updateObjectSection hook)
    if (!g.objList.empty()) {
        double c0 = qpcMs();
        for (size_t i = 0; i < g.objList.size();) {
            ObjEntry& e = g.objList[i];
            if (!g_set.objects || g.serial - e.ot.lastMove > 120 ||
                (g_objStride > 1 && ((reinterpret_cast<uintptr_t>(e.obj) >> 4) % g_objStride) != 0)) {
                objRemoveAt(i); // stopped moving a while ago, or thinned out by the cost control
                continue;
            }
            if (!field<bool>(e.obj, ccnode::visible)) {
                e.ot.tr.n = 0; // off screen: skip it, and start clean when it comes back
                ++i;
                continue;
            }
            pushSample(e.ot.tr, Kind::Object, readObjVals(e.obj), advance);
            ++i;
        }
        g_frameCostMs += qpcMs() - c0;
    }
    if ((g.serial & 255) == 0) {
        for (auto it = g.tracks.begin(); it != g.tracks.end();) {
            if (g.serial - it->second.seen > 240) it = g.tracks.erase(it);
            else ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// 2) prediction
// ---------------------------------------------------------------------------
float predictOffset(Track const& tr, float Vals::*a, double h, bool allowAccel, bool wrapAngle) {
    if (tr.n < 2) return 0.f;
    auto delta = [&](Sample const& p, Sample const& q) {
        float d = q.v.*a - p.v.*a;
        if (wrapAngle) {
            while (d > 180.f) d -= 360.f;
            while (d < -180.f) d += 360.f;
        }
        return d;
    };
    Sample const& s2 = tr.s[tr.n - 1];
    Sample const& s1 = tr.s[tr.n - 2];
    double dt2 = s2.t - s1.t;
    if (dt2 <= 1e-9) return 0.f;
    float d2 = delta(s1, s2);
    double v2 = d2 / dt2;

    if (h <= 0) {
        if (-h <= dt2 || tr.n < 3) return static_cast<float>(v2 * std::max(h, -dt2));
        Sample const& s0 = tr.s[tr.n - 3];
        double dt1 = s1.t - s0.t;
        if (dt1 <= 1e-9) return static_cast<float>(-d2);
        double v1 = delta(s0, s1) / dt1;
        return static_cast<float>(-d2 + v1 * std::max(h + dt2, -dt1));
    }

    if (v2 == 0.0) return 0.f;
    double vp = v2;
    if (allowAccel && tr.n >= 3) {
        Sample const& s0 = tr.s[tr.n - 3];
        double dt1 = s1.t - s0.t;
        if (dt1 > 1e-9) {
            double v1 = delta(s0, s1) / dt1;
            if (v1 != 0.0 && (v1 > 0) == (v2 > 0)) {
                double acc = (v2 - v1) / ((dt1 + dt2) * 0.5);
                vp = v2 + acc * h * 0.5;
                if ((vp > 0) != (v2 > 0)) vp = 0.0;
                if (std::fabs(vp) > std::fabs(v2) * 2.0) vp = v2 * 2.0;
            }
        }
    }
    return static_cast<float>(vp * h);
}

// ---------------------------------------------------------------------------
// 3) apply before drawing / 4) restore after drawing
// ---------------------------------------------------------------------------
bool layerIsRunning(void* layer) {
    void* dir = cc.sharedDirector();
    if (!dir) return false;
    void* scene = cc.getRunningScene(dir);
    if (!scene) return false;
    void* ch = field<void*>(scene, ccnode::children);
    unsigned c = arrCount(ch);
    for (unsigned i = 0; i < c; i++)
        if (arrAt(ch, i) == layer) return true;
    return false;
}


// ---------------------------------------------------------------------------
// Trails: the icon is drawn ahead of its physics position, but the game only extends the trails on
// physics ticks, so between ticks the trail ended a little behind the icon. Right before drawing we give
// each trail a temporary head at the icon's drawn position and take it away after drawing (like the
// icon itself). Physics and the stored trail points are never changed.
//  - normal trail (cocos2d::CCMotionStreak): one extra point is added to the vertex list just for drawing
//  - wave trail (HardStreak): its head is its own position, so the position is moved and the trail is
//    rebuilt once with the game's own updateStroke (the game rebuilds it again next frame anyway)
// Only on the "ahead" side (h > 0): pulling a head backwards could fold it over a corner.
// ---------------------------------------------------------------------------
struct TrailApplied { void* node; uint32_t n; }; // motion streak and its point count before our extra point
std::vector<TrailApplied> g_trailApplied;
std::unordered_map<void*, uint8_t> g_trailKind; // 1 = CCMotionStreak, 2 = HardStreak, 0 = something else
bool g_trailsBroken = false;
struct PlayerShift { void* player; float dx, dy; };
PlayerShift g_shift[2];
int g_shiftN = 0;
uint64_t g_trailsDrawn = 0;

void clearTrailKinds() { g_trailKind.clear(); }

uint8_t trailKind(void* n) {
    auto it = g_trailKind.find(n);
    if (it != g_trailKind.end()) return it->second;
    const char* name = rttiName(n);
    uint8_t k = 0;
    if (name && std::strcmp(name, ".?AVCCMotionStreak@cocos2d@@") == 0) k = 1;
    else if (name && std::strcmp(name, ".?AVHardStreak@@") == 0) k = 2;
    if (g_trailKind.size() > 64) g_trailKind.clear();
    g_trailKind[n] = k;
    return k;
}

// normal trail / ship fire: one temporary point at the head, only for drawing
void applyMotionStreak(void* st, float dx, float dy) {
    if (!field<bool>(st, streak::started) || !field<bool>(st, streak::stroking)) return;
    uint32_t n = field<uint32_t>(st, streak::nuPoints), maxN = field<uint32_t>(st, streak::maxPoints);
    if (n < 2 || n >= maxN) return; // no room for a temporary point: leave it as the game made it
    auto pts = field<CCPoint*>(st, streak::points);
    auto v = field<float*>(st, streak::vertices);
    auto col = field<uint8_t*>(st, streak::colors);
    auto tex = field<float*>(st, streak::texCoords);
    if (!pts || !v || !col || !tex) return;
    // head in the streak's own (local) space: where the game puts the head, plus the icon's shift
    CCPoint head{field<float>(st, streak::positionR) + dx - field<float>(st, ccnode::position),
                 field<float>(st, streak::positionR + 4) + dy - field<float>(st, ccnode::position + 4)};
    CCPoint prev = pts[n - 1];
    float ex = head.x - prev.x, ey = head.y - prev.y;
    float len = std::sqrt(ex * ex + ey * ey);
    if (len < 0.05f) return;
    float maxSeg = field<float>(st, streak::maxSeg);
    if (maxSeg > 0 && len > maxSeg) return; // a teleport: the game restarts the trail there
    // never fold back over the last segment (it would draw a spike)
    CCPoint pp = pts[n - 2];
    if ((prev.x - pp.x) * ex + (prev.y - pp.y) * ey < 0) return;
    float w = field<float>(st, streak::width) * 0.5f;
    float px = -ey / len * w, py = ex / len * w;
    // same side order as the game's own vertices for the previous point (no twisted last quad)
    const float* pv = v + 4 * (n - 1);
    if ((pv[0] - prev.x) * px + (pv[1] - prev.y) * py < 0) { px = -px; py = -py; }
    float* hv = v + 4 * n;
    hv[0] = head.x + px; hv[1] = head.y + py;
    hv[2] = head.x - px; hv[3] = head.y - py;
    std::memcpy(col + 8 * n, col + 8 * (n - 1), 8);
    std::memcpy(tex + 4 * n, tex + 4 * (n - 1), 4 * sizeof(float));
    field<uint32_t>(st, streak::nuPoints) = n + 1;
    cc.retain(st);
    g_trailApplied.push_back({st, n});
}

void applyTrails() {
    for (int i = 0; i < g_shiftN; i++) {
        const PlayerShift& s = g_shift[i];
        if (std::fabs(s.dx) + std::fabs(s.dy) < 1e-3f || std::fabs(s.dx) > 40.f || std::fabs(s.dy) > 40.f) continue;
        void* parent = field<void*>(s.player, ccnode::parent);
        static bool loggedOnce = false;
        if (!loggedOnce) {
            loggedOnce = true;
            void* st0 = field<void*>(s.player, gd::regularTrail);
            void* hs0 = field<void*>(s.player, gd::waveTrail);
            logf("trails: normal %s (same layer as the icon: %s), wave %s (same layer: %s)",
                st0 && rttiName(st0) ? rttiName(st0) : "?", st0 && field<void*>(st0, ccnode::parent) == parent ? "yes" : "no",
                hs0 && rttiName(hs0) ? rttiName(hs0) : "?", hs0 && field<void*>(hs0, ccnode::parent) == parent ? "yes" : "no");
        }
        // only when the trail lives in the same space as the icon (so the icon's shift applies to it)
        for (size_t off : {gd::regularTrail, gd::shipStreak}) {
            void* st = field<void*>(s.player, off);
            if (st && field<void*>(st, ccnode::parent) == parent && field<bool>(st, ccnode::visible) && trailKind(st) == 1)
                applyMotionStreak(st, s.dx, s.dy);
        }
    }
}

void applyTrailsSEH() {
    __try {
        applyTrails();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_trailsBroken = true;
        logf("trail smoothing failed (%08lx): turned off until restart", GetExceptionCode());
    }
}

void restoreTrails() {
    for (auto& a : g_trailApplied) {
        // only if the game did not change the trail in between
        if (field<uint32_t>(a.node, streak::nuPoints) == a.n + 1) field<uint32_t>(a.node, streak::nuPoints) = a.n;
        cc.release(a.node);
    }
    g_trailApplied.clear();
}

// ---- wave trail: the game itself rebuilds it every frame (HardStreak::updateStroke, from the scheduler,
// after the physics of that frame). We hook that call and move only its head (m_currentPoint) by the
// icon's shift for the frame while the game builds it, then put the value back. No extra rebuild, no
// cost; the stored corners stay where they are. Physics never reads the drawing.
double frameH(void* layer, double* stepOut);
bool predictPlayerShift(void* player, double h, float& dx, float& dy);
using FnStroke = void (*)(void*, float);
FnStroke o_updateStroke = nullptr;
bool g_strokeHooked = false;

bool waveShift(void* hs, float& dx, float& dy) {
    void* layer = g.layer;
    if (!layer || trailKind(hs) != 2 || !field<bool>(hs, gd::hsDrawStreak)) return false;
    double h = frameH(layer, nullptr);
    if (h <= 0) return false;
    for (size_t off : {gd::player1, gd::player2}) {
        void* p = field<void*>(layer, off);
        if (!p || field<void*>(p, gd::waveTrail) != hs) continue;
        if (field<void*>(hs, ccnode::parent) != field<void*>(p, ccnode::parent)) return false;
        if (!predictPlayerShift(p, h, dx, dy)) return false;
        return std::fabs(dx) + std::fabs(dy) > 1e-3f && std::fabs(dx) < 40.f && std::fabs(dy) < 40.f;
    }
    return false;
}

bool waveShiftSEH(void* hs, float& dx, float& dy) {
    __try {
        return waveShift(hs, dx, dy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_trailsBroken = true;
        logf("wave trail smoothing failed (%08lx): turned off until restart", GetExceptionCode());
        return false;
    }
}

void h_updateStroke(void* hs, float dt) {
    float dx = 0, dy = 0;
    if (g_set.enabled && g_set.trails && !g_trailsBroken && g.hooked && waveShiftSEH(hs, dx, dy)) {
        CCPoint cp = field<CCPoint>(hs, gd::hsCurrentPoint);
        field<CCPoint>(hs, gd::hsCurrentPoint) = CCPoint{cp.x + dx, cp.y + dy};
        o_updateStroke(hs, dt);
        field<CCPoint>(hs, gd::hsCurrentPoint) = cp;
        g_trailsDrawn++;
        return;
    }
    o_updateStroke(hs, dt);
}

void restoreAll() {
    restoreTrails();
    double r0 = g.applied.empty() ? 0.0 : qpcMs();
    for (auto& a : g.applied) {
        Vals cur = readVals(a.node);
        if (std::fabs(cur.x - a.shown.x) < 1e-3f && std::fabs(cur.y - a.shown.y) < 1e-3f)
            nodeSetPosition(a.node, a.orig.x, a.orig.y);
        if (usesCameraSpace(a.kind)) {
            if (a.orig.sx != a.shown.sx && std::fabs(cur.sx - a.shown.sx) < 1e-5f) nodeSetScaleX(a.node, a.orig.sx);
            if (a.orig.sy != a.shown.sy && std::fabs(cur.sy - a.shown.sy) < 1e-5f) nodeSetScaleY(a.node, a.orig.sy);
        }
        if (a.kind == Kind::Object && std::fabs(cur.rot - a.shown.rot) < 1e-3f && a.orig.rot != a.shown.rot)
            nodeSetRotation(a.node, a.orig.rot);
        if (a.kind == Kind::Player) {
            if (std::fabs(cur.rot - a.shown.rot) < 1e-3f && a.orig.rot != a.shown.rot)
                nodeSetRotation(a.node, a.orig.rot);
            auto it = g.tracks.find(a.node);
            if (it != g.tracks.end()) {
                it->second.restoredRot = field<float>(a.node, ccnode::rotationX);
                it->second.hasRestoredRot = true;
            }
        }
        cc.release(a.node);
    }
    g.applied.clear();
    if (r0 > 0) g_frameCostMs += qpcMs() - r0;
}

void publishStatus(double h, double stepU) {
    ULONGLONG now = GetTickCount64();
    if (!g.statStart) g.statStart = now;
    if (now - g.lastBeat < 250) return;
    double secs = (now - g.statStart) / 1000.0;
    if (g_status) {
        g_status->heartbeat = now;
        g_status->enabled = g_set.enabled;
        g_status->mode = g_set.interpolate ? 1 : 0;
        g_status->inLevel = g.layer != nullptr;
        if (secs > 0 && g.frames) {
            g_status->fps = static_cast<float>(g.frames / secs);
            g_status->ticksPerFrame = static_cast<float>(double(g.samples) / g.frames);
            g_status->avgOffset = static_cast<float>(g.hSum / g.frames / (stepU > 0 ? stepU : 0.25));
            g_status->nodes = static_cast<uint32_t>(g.moved / g.frames);
            g_status->objects = static_cast<uint32_t>(g.movedObjs / g.frames);
        } else if (secs > 0) {
            g_status->fps = 0;
        }
    }
    if (g_set.debug && g.frames && ++g.debugFrames % 8 == 0)
        logf("[stats] fps=%.0f ticks/frame=%.3f avg offset=%.3f ticks nodes=%llu objects=%llu skipped=%llu tracked=%zu cost=%.2fms stride=%u",
            g.frames / secs, double(g.samples) / g.frames, g.hSum / g.frames / 0.25,
            (unsigned long long)(g.moved / g.frames), (unsigned long long)(g.movedObjs / g.frames), (unsigned long long)(g.skippedObjs / g.frames), g.objList.size(), g_costEma, g_objStride);
    g.lastBeat = now;
    g.statStart = now;
    g.frames = g.samples = g.moved = g.movedObjs = g.skippedObjs = 0;
    g.hSum = 0;
    (void)h;
}

// How far the drawing is from the last physics tick, in 1/60 s units (negative = behind in interpolation)
double frameH(void* layer, double* stepOut) {
    double extra = field<double>(layer, gd::extraDelta);
    float tw = field<float>(layer, gd::timeWarp);
    double stepU = 0.25 * std::min(1.0, tw > 0 ? double(tw) : 1.0);
    double h = extra * 60.0;
    if (g_set.interpolate) h -= 0.5 * stepU;
    if (stepOut) *stepOut = stepU;
    return std::clamp(h, -1.5 * stepU, 0.75 * stepU);
}

// The icon's shift for this frame, computed exactly like applyAll does for Kind::Player
bool predictPlayerShift(void* player, double h, float& dx, float& dy) {
    auto it = g.tracks.find(player);
    if (it == g.tracks.end()) return false;
    Track& tr = it->second;
    if (tr.seen != g.serial || tr.n < 2) return false;
    if (!nearlySame(readVals(player), tr.s[tr.n - 1].v)) return false;
    dx = predictOffset(tr, &Vals::x, h, false, false);
    dy = predictOffset(tr, &Vals::y, h, g_set.smartPredict, false);
    return true;
}

void applyAll() {
    restoreAll();
    g_shiftN = 0;
    maybeReloadIni();
    double h = 0, stepU = 0.25;
    if (g_set.enabled && g.layer) {
        if (!layerIsRunning(g.layer)) {
            g.layer = nullptr;
            g.tracks.clear();
            clearObjs();
        }
    }
    if (g_set.enabled && g.layer) {
        void* layer = g.layer;
        h = frameH(layer, &stepU);
        g.hSum += h;

        collectNodes(layer, g.nodes);
        for (auto& [node, kind] : g.nodes) {
            auto it = g.tracks.find(node);
            if (it == g.tracks.end()) continue;
            Track& tr = it->second;
            if (tr.seen != g.serial || tr.n < 2) continue;
            Vals cur = readVals(node);
            Vals model = toModel(kind, cur);
            if (!nearlySame(model, tr.s[tr.n - 1].v)) continue;

            bool accel = g_set.smartPredict;
            bool cam = usesCameraSpace(kind);
            Vals pm = model; // predicted, in the same space as the samples
            pm.x += predictOffset(tr, &Vals::x, h, accel && kind != Kind::Player, false);
            pm.y += predictOffset(tr, &Vals::y, h, accel, false);
            if (cam) {
                pm.sx += predictOffset(tr, &Vals::sx, h, accel, false);
                pm.sy += predictOffset(tr, &Vals::sy, h, accel, false);
            }
            Vals shown = cur;
            if (cam) {
                // rebuild the position from (worldOffset, scale); add only the difference so a
                // node that is not moving stays exactly where GD put it (no float round trip)
                Vals a = fromModel(kind, model), b = fromModel(kind, pm);
                shown.x += b.x - a.x;
                shown.y += b.y - a.y;
                shown.sx = pm.sx;
                shown.sy = pm.sy;
            } else {
                shown.x = pm.x;
                shown.y = pm.y;
            }
            if (kind == Kind::Player && g_set.rotation && tr.actionRotFrames == 0 &&
                std::fabs(cur.rot - tr.s[tr.n - 1].v.rot) < 1e-4f)
                shown.rot += predictOffset(tr, &Vals::rot, h, false, true);

            cc.retain(node);
            if (shown.x != cur.x || shown.y != cur.y) nodeSetPosition(node, shown.x, shown.y);
            if (cam) {
                if (shown.sx != cur.sx) nodeSetScaleX(node, shown.sx);
                if (shown.sy != cur.sy) nodeSetScaleY(node, shown.sy);
            }
            if (shown.rot != cur.rot) nodeSetRotation(node, shown.rot);
            g.applied.push_back({node, kind, cur, shown});
            if (kind == Kind::Player && g_shiftN < 2) g_shift[g_shiftN++] = {node, shown.x - cur.x, shown.y - cur.y};
            g.moved++;
        }

        // objects moved by triggers
        if (g_set.objects) {
            double c0 = qpcMs();
            for (auto& entry : g.objList) {
                void* obj = entry.obj;
                ObjTrack& ot = entry.ot;
                Track& tr = ot.tr;
                if (tr.seen != g.serial || tr.n < 2) continue;
                if (!field<bool>(obj, ccnode::visible) || !field<void*>(obj, ccnode::parent)) continue;
                Vals cur = readVals(obj); // sprite
                Vals const& model = tr.s[tr.n - 1].v;
                // Only when the sprite sits exactly on its physics position. Objects animated by
                // Area / enter effects, keyframes, etc. have an extra visual offset that changes every
                // frame; moving those would make them shake, so they are left alone.
                if (std::fabs(cur.x - model.x) > 0.05f || std::fabs(cur.y - model.y) > 0.05f) {
                    g.skippedObjs++;
                    continue;
                }
                // Only smooth steady motion (Move triggers with a duration). Many decorations are
                // animated with instant moves/teleports: one big jump, then nothing. Predicting from a
                // jump would throw the piece far away, so both of the last two ticks must agree.
                if (tr.n < 3 || ot.lastMove != g.serial) continue;
                auto steady = [&](float Vals::*a) {
                    double dtA = tr.s[1].t - tr.s[0].t, dtB = tr.s[2].t - tr.s[1].t;
                    if (dtA <= 1e-9 || dtB <= 1e-9) return false;
                    double va = (tr.s[1].v.*a - tr.s[0].v.*a) / dtA;
                    double vb = (tr.s[2].v.*a - tr.s[1].v.*a) / dtB;
                    if (std::fabs(vb) < 1e-6) return false;      // not moving on this axis
                    if (std::fabs(va) < 1e-6) return false;      // jump right after standing still
                    // a sudden huge change while moving is also a jump; anything else (speeding up,
                    // slowing down, turning around like the camera does) is smoothed exactly like
                    // the camera, so objects that follow the camera/player stay glued to it
                    return std::fabs(vb) < std::fabs(va) * 8.0;
                };
                float dx = steady(&Vals::x) ? predictOffset(tr, &Vals::x, h, g_set.smartPredict, false) : 0.f;
                float dy = steady(&Vals::y) ? predictOffset(tr, &Vals::y, h, g_set.smartPredict, false) : 0.f;
                // never move a piece more than a few units: anything bigger is not smooth motion
                if (std::fabs(dx) > 12.f || std::fabs(dy) > 12.f) { g.skippedObjs++; continue; }
                Vals shown = cur;
                shown.x += dx;
                shown.y += dy;
                if (shown.x == cur.x && shown.y == cur.y) continue;
                cc.retain(obj);
                // GameObject::setPosition / setRotation also move its glow and color sprites,
                // and only touch the sprite: the physics position (doubles) is never changed
                if (shown.x != cur.x || shown.y != cur.y) nodeSetPosition(obj, shown.x, shown.y);
                g.applied.push_back({obj, Kind::Object, cur, shown});
                g.movedObjs++;
            }
            g_frameCostMs += qpcMs() - c0;
        }

        // trails: a temporary head where the icon is drawn (only when the icon is ahead, h > 0)
        if (g_set.trails && !g_trailsBroken && h > 0 && g_shiftN > 0) {
            double c0 = qpcMs();
            size_t before = g_trailApplied.size();
            applyTrailsSEH();
            g_trailsDrawn += g_trailApplied.size() - before;
            g_frameCostMs += qpcMs() - c0;
        }
    }
    g.frames++;
    updateCostControl();
    publishStatus(h, stepU);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
using FnF = void (*)(void*, float);
using FnV = void (*)(void*);
FnF o_updateCamera = nullptr, o_update = nullptr, o_schedulerUpdate = nullptr;
FnV o_drawScene = nullptr;

void h_updateCamera(void* self, float dt) {
    o_updateCamera(self, dt);
    onCameraSample(self, dt);
}

using FnObj = void (*)(void*, void*);
FnObj o_updateObjectSection = nullptr;

void h_updateObjectSection(void* self, void* obj) {
    o_updateObjectSection(self, obj);
    if (obj && self == g.layer && g_set.enabled && g_set.objects) {
        if (!objectWanted(obj)) return; // off screen, or not part of the subset the cost control keeps
        auto found = g.objIndex.find(obj);
        if (found != g.objIndex.end()) {
            g.objList[found->second].ot.lastMove = g.serial + 1; // the tick being simulated right now
            return;
        }
        if (g.objList.size() >= kMaxTracked) return; // safety limit
        cc.retain(obj);
        g.objIndex.emplace(obj, static_cast<uint32_t>(g.objList.size()));
        g.objList.push_back({obj, ObjTrack{}});
        g.objList.back().ot.lastMove = g.serial + 1;
    }
}

void h_update(void* self, float dt) {
    if (self == g.layer) {
        for (size_t off : {gd::player1, gd::player2}) {
            void* p = field<void*>(self, off);
            if (!p) continue;
            auto it = g.tracks.find(p);
            if (it == g.tracks.end()) continue;
            Track& tr = it->second;
            float r = field<float>(p, ccnode::rotationX);
            if (tr.hasRestoredRot && std::fabs(r - tr.restoredRot) > 1e-4f) tr.actionRotFrames = 30;
            else if (tr.actionRotFrames > 0) tr.actionRotFrames--;
            tr.hasRestoredRot = false;
        }
    }
    o_update(self, dt);
}

void h_schedulerUpdate(void* self, float dt) {
    restoreAll();
    o_schedulerUpdate(self, dt);
    applyAll();
}

void h_drawScene(void* self) {
    o_drawScene(self);
    restoreAll();
}

// Is the game sitting on its main menu? (loading finished, Geode done hooking)
bool atMenuSEH() {
    __try {
        void* dir = cc.sharedDirector();
        if (!dir) return false;
        void* scene = cc.getRunningScene(dir);
        if (!scene) return false;
        void* ch = field<void*>(scene, ccnode::children);
        unsigned c = arrCount(ch);
        for (unsigned i = 0; i < c && i < 16; i++) {
            const char* n = rttiName(arrAt(ch, i));
            if (n && (std::strcmp(n, ".?AVMenuLayer@@") == 0 || std::strcmp(n, ".?AVPlayLayer@@") == 0 ||
                      std::strcmp(n, ".?AVLevelSelectLayer@@") == 0))
                return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

DWORD WINAPI initBody() {
    // wait for libcocos2d
    for (int i = 0; i < 600 && !GetModuleHandleW(L"libcocos2d.dll"); i++) Sleep(100);
    if (!resolveCocos()) {
        setState(KFE_STATE_HOOK_FAILED, "Could not find the cocos2d functions. Mod disabled.");
        return 0;
    }
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    unsigned char* camAddr = base + gd::updateCamera;
    unsigned char* updAddr = base + gd::update;
    auto startsOk = [](unsigned char* a, const unsigned char* sig, size_t n) {
        return std::memcmp(a, sig, n) == 0 || a[0] == 0xE9;
    };
    if (!startsOk(camAddr, gd::updateCameraSig, sizeof(gd::updateCameraSig)) ||
        !startsOk(updAddr, gd::updateSig, sizeof(gd::updateSig))) {
        setState(KFE_STATE_WRONG_VERSION,
            "This Geometry Dash is not the supported build (2026-10-20 patch) or another mod already hooked it. Mod disabled.");
        return 0;
    }
    // let the game (and Geode, if installed) finish loading before touching anything
    for (int i = 0; i < 3000 && !atMenuSEH(); i++) Sleep(100);
    Sleep(300);
    // check again. Kuang Menu (and other MinHook mods) put a jump at the start of updateCamera: MinHook chains on
    // top of that without problems, so a first byte of E9 (jump) is accepted. Only the Geode version of this very
    // mod would smooth twice, and that one is looked for by name.
    bool geodeCopy = GetModuleHandleW(L"kuang.frame-extrapolation.dll") != nullptr;
    if (geodeCopy || !startsOk(camAddr, gd::updateCameraSig, sizeof(gd::updateCameraSig)) ||
        !startsOk(updAddr, gd::updateSig, sizeof(gd::updateSig))) {
        setState(KFE_STATE_HOOK_FAILED,
            "Another mod already hooks the camera (remove kuang.frame-extrapolation.geode). Mod disabled.");
        return 0;
    }

    logf("start of updateCamera: %02x %02x %02x %02x %02x (%s)", camAddr[0], camAddr[1], camAddr[2], camAddr[3], camAddr[4],
        camAddr[0] == 0xE9 ? "another mod hooked it first, chaining" : "original");
    if (MH_Initialize() != MH_OK) {
        setState(KFE_STATE_HOOK_FAILED, "MinHook failed to initialize. Mod disabled.");
        return 0;
    }
    bool ok = MH_CreateHook(camAddr, reinterpret_cast<void*>(&h_updateCamera), reinterpret_cast<void**>(&o_updateCamera)) == MH_OK &&
        MH_CreateHook(updAddr, reinterpret_cast<void*>(&h_update), reinterpret_cast<void**>(&o_update)) == MH_OK &&
        MH_CreateHook(cc.schedulerUpdate, reinterpret_cast<void*>(&h_schedulerUpdate), reinterpret_cast<void**>(&o_schedulerUpdate)) == MH_OK &&
        MH_CreateHook(cc.drawScene, reinterpret_cast<void*>(&h_drawScene), reinterpret_cast<void**>(&o_drawScene)) == MH_OK;
    unsigned char* secAddr = base + gd::updateObjectSection;
    if (ok && std::memcmp(secAddr, gd::updateObjectSectionSig, sizeof(gd::updateObjectSectionSig)) == 0) {
        if (MH_CreateHook(secAddr, reinterpret_cast<void*>(&h_updateObjectSection),
                reinterpret_cast<void**>(&o_updateObjectSection)) != MH_OK)
            logf("could not hook updateObjectSection: moving objects will not be smoothed");
    } else if (ok) {
        logf("updateObjectSection signature mismatch: moving objects will not be smoothed");
    }
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        setState(KFE_STATE_HOOK_FAILED, "Could not install the hooks. Mod disabled.");
        return 0;
    }
    // wave trail: hook the game's own per-frame rebuild, only if it is where we expect
    unsigned char* strokeAddr = base + gd::hardStreakUpdateStroke;
    if (std::memcmp(strokeAddr, gd::hardStreakUpdateStrokeSig, sizeof(gd::hardStreakUpdateStrokeSig)) == 0 &&
        MH_CreateHook(strokeAddr, reinterpret_cast<void*>(&h_updateStroke), reinterpret_cast<void**>(&o_updateStroke)) == MH_OK &&
        MH_EnableHook(strokeAddr) == MH_OK)
        g_strokeHooked = true;
    else
        logf("HardStreak::updateStroke not found in this build: the wave trail will not be smoothed (the normal trail still is)");
    g.hooked = true;
    setState(KFE_STATE_ACTIVE, "Active. Hooks installed for the 2026-10-20 Geometry Dash patch.");
    return 0;
}

} // namespace

DWORD WINAPI initThread(LPVOID) {
    initBody();
    // when it did not end Active the heartbeat would stop and the menu would show "Starting" forever:
    // keep it beating so the menu shows the real message
    while (g_status && g_status->state != KFE_STATE_ACTIVE) {
        g_status->heartbeat = GetTickCount64();
        Sleep(500);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        std::wstring p = path;
        g_dir = p.substr(0, p.find_last_of(L"\\/") + 1);
        g_log = _wfopen((g_dir + KFE_LOG_NAME).c_str(), L"w");
        logf("Kuang Frame Extrapolation %s loaded", KFE_VERSION_STR);
        writeDefaultIni();
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(iniPath().c_str(), GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
        loadIni();
        openStatus();
        if (HANDLE t = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
