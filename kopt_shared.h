// Shared between kopt.dll (inside Geometry Dash) and KuangOptimizer.exe.
#pragma once
#include <cstdint>

#define KOPT_NAME            L"Kuang Optimizer"
#define KOPT_VERSION_STR     "1.0.0"
#define KOPT_STATUS_MAPPING  L"Local\\KuangOptimizerStatus"
#define KOPT_INI_NAME        L"kopt.ini"
#define KOPT_LOG_NAME        L"kopt.log"
#define KOPT_DLL_NAME        L"kopt.dll"

constexpr uint32_t KOPT_STATUS_MAGIC = 0x54504F4B; // 'KOPT'

enum KoptState : uint32_t {
    KOPT_STATE_STARTING = 0,
    KOPT_STATE_ACTIVE   = 1,
    KOPT_STATE_ERROR    = 2,
};

#pragma pack(push, 8)
struct KoptStatus {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t state;
    uint64_t heartbeat;       // GetTickCount64() of the last update
    uint32_t enabled;
    uint32_t priorityClass;   // GetPriorityClass of GD
    uint64_t affinity;        // process affinity mask
    uint64_t systemAffinity;
    uint32_t timerRes;        // current timer resolution, 100 ns units
    int32_t  gpuClass;        // D3DKMT scheduling class (2 normal, 4 high), -1 unknown
    int32_t  ioLevel;         // 0..3, -1 unknown
    uint32_t boostDisabled;
    uint32_t flushGpu;        // flushing the GPU queue right now
    uint32_t vsyncOff;        // V-Sync was forced off
    uint32_t corrections;     // watchdog fixes since start
    float    fps;
    char     plan[96];        // active power plan name
    char     message[192];
};
#pragma pack(pop)
