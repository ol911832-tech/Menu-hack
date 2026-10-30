// Shared between kfe.dll (the mod) and the Kuang Frame Extrapolation app.
#pragma once
#include <cstdint>

#define KFE_NAME            L"Kuang Frame Extrapolation"
#define KFE_VERSION_STR     "1.2.1"
#define KFE_STATUS_MAPPING  L"Local\\KuangFrameExtrapolationStatus"
#define KFE_INI_NAME        L"kfe.ini"
#define KFE_LOG_NAME        L"kfe.log"
#define KFE_DLL_NAME        L"kfe.dll"
#define KFE_PROXY_NAME      L"winmm.dll"
// marker compiled into winmm.dll so the app knows the proxy is ours
#define KFE_PROXY_MARKER    "KUANG_FRAME_EXTRAPOLATION_PROXY_V1"

constexpr uint32_t KFE_STATUS_MAGIC = 0x3145464B; // 'KFE1'

enum KfeHookState : uint32_t {
    KFE_STATE_STARTING      = 0, // dll loaded, waiting for the main menu
    KFE_STATE_ACTIVE        = 1, // hooks installed
    KFE_STATE_WRONG_VERSION = 2, // GeometryDash.exe is not 2.2091
    KFE_STATE_HOOK_FAILED   = 3,
};

#pragma pack(push, 8)
struct KfeStatus {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t state;        // KfeHookState
    uint64_t heartbeat;    // GetTickCount64() of the last update (updated ~4x per second)
    uint32_t enabled;      // current value from kfe.ini
    uint32_t mode;         // 0 = extrapolation, 1 = interpolation
    uint32_t inLevel;      // 1 while a PlayLayer is being smoothed
    float    fps;          // rendered frames per second
    float    ticksPerFrame;
    float    avgOffset;    // average correction, in physics ticks (-0.5..0.5)
    uint32_t nodes;        // nodes moved per frame
    char     message[192]; // human readable status / error
    uint32_t objects;      // moving objects smoothed per frame
};
#pragma pack(pop)
