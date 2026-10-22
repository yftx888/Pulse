// Console profiler: --samples N (default 60), --compact, --brief, --url URL,
// --interval-ms 500|1000|2000, --switch-every N, --slow-ms 300|500|800, --fps,
// --no-probe, --process-count-only, --gpu (five direct D3DKMT diagnostic rounds).
// Timings are per actual collection, not per cached Sample() call.
// g++ -std=c++17 -O2 -municode -static -finput-charset=UTF-8 -fexec-charset=UTF-8
//   -o metrics_selftest.exe src/metrics_selftest.cpp src/metrics.cpp src/netprobe.cpp src/fpsmon.cpp
//   -lwinhttp -lpowrprof -lpdh -liphlpapi -lws2_32 -lsetupapi

#include "metrics.h"
#include "netprobe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#ifndef _O_U16TEXT
#define _O_U16TEXT 0x00020000
#endif

namespace {

// Windows SDK shared/d3dkmthk.h (Microsoft win32metadata, RecompiledIdlHeaders):
// https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/shared/d3dkmthk.h
// Local x64 ABI declarations: MinGW's SDK does not provide these newer types.
// KMTQUERYADAPTERINFOTYPE: ADAPTERTYPE = 15, ADAPTERPERFDATA = 62 (WDDM 2.4+).
constexpr UINT kPulseKmtAdapterType = 15;
constexpr UINT kPulseKmtAdapterPerfData = 62;
// ADAPTERTYPE's UINT bitfield: RenderSupported is bit 0; SoftwareDevice is
// the third bit (zero-based bit 2), not bit 3. Read masks to avoid bitfield ABI issues.
constexpr UINT kPulseKmtRenderSupported = 1u << 0;
constexpr UINT kPulseKmtSoftwareDevice = 1u << 2;
struct PulseKmtAdapterInfo {
    UINT hAdapter; // D3DKMT_HANDLE is a 32-bit UINT, not a Win32 HANDLE.
    LUID AdapterLuid;
    ULONG NumOfSources;
    BOOL bPrecisePresentRegionsPreferred;
};
struct PulseKmtEnumAdapters2 {
    ULONG NumAdapters;
    PulseKmtAdapterInfo* pAdapters;
};
struct PulseKmtQueryAdapterInfo {
    UINT hAdapter;
    UINT Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};
struct PulseKmtCloseAdapter { UINT hAdapter; };
struct PulseKmtAdapterType { UINT Value; };
struct PulseKmtAdapterPerfData {
    UINT32 PhysicalAdapterIndex;
    alignas(8) ULONGLONG MemoryFrequency;
    ULONGLONG MaxMemoryFrequency;
    ULONGLONG MaxMemoryFrequencyOC;
    ULONGLONG MemoryBandwidth;
    ULONGLONG PCIEBandwidth;
    ULONG FanRPM;
    ULONG Power;       // 1 = 0.1% (one thousandth of full power).
    ULONG Temperature; // 1 = 0.1 degrees Celsius; 0 means unavailable.
    UCHAR PowerStateOverride;
};
static_assert(sizeof(PulseKmtAdapterInfo) == 20, "D3DKMT_ADAPTERINFO size");
static_assert(offsetof(PulseKmtAdapterInfo, AdapterLuid) == 4, "AdapterLuid offset");
static_assert(offsetof(PulseKmtAdapterInfo, NumOfSources) == 12, "NumOfSources offset");
static_assert(offsetof(PulseKmtAdapterInfo, bPrecisePresentRegionsPreferred) == 16, "PrecisePresent offset");
static_assert(sizeof(PulseKmtEnumAdapters2) == 16, "D3DKMT_ENUMADAPTERS2 size");
static_assert(offsetof(PulseKmtEnumAdapters2, pAdapters) == 8, "pAdapters offset");
static_assert(sizeof(PulseKmtQueryAdapterInfo) == 24, "D3DKMT_QUERYADAPTERINFO size");
static_assert(offsetof(PulseKmtQueryAdapterInfo, Type) == 4, "Query Type offset");
static_assert(offsetof(PulseKmtQueryAdapterInfo, pPrivateDriverData) == 8, "PrivateDriverData offset");
static_assert(offsetof(PulseKmtQueryAdapterInfo, PrivateDriverDataSize) == 16, "PrivateDriverDataSize offset");
static_assert(sizeof(PulseKmtCloseAdapter) == 4, "D3DKMT_CLOSEADAPTER size");
static_assert(sizeof(PulseKmtAdapterType) == 4, "D3DKMT_ADAPTERTYPE size");
static_assert(sizeof(PulseKmtAdapterPerfData) == 64, "D3DKMT_ADAPTER_PERFDATA size");
static_assert(alignof(PulseKmtAdapterPerfData) == 8, "ADAPTER_PERFDATA alignment");
static_assert(offsetof(PulseKmtAdapterPerfData, MemoryFrequency) == 8, "MemoryFrequency offset");
static_assert(offsetof(PulseKmtAdapterPerfData, PCIEBandwidth) == 40, "PCIEBandwidth offset");
static_assert(offsetof(PulseKmtAdapterPerfData, FanRPM) == 48, "FanRPM offset");
static_assert(offsetof(PulseKmtAdapterPerfData, Power) == 52, "Power offset");
static_assert(offsetof(PulseKmtAdapterPerfData, Temperature) == 56, "Temperature offset");
static_assert(offsetof(PulseKmtAdapterPerfData, PowerStateOverride) == 60, "PowerStateOverride offset");

struct PulseKmtApi {
    // NTSTATUS is a signed 32-bit LONG; NT_SUCCESS(status) means status >= 0.
    using EnumFn = LONG(WINAPI*)(const PulseKmtEnumAdapters2*);
    using QueryFn = LONG(WINAPI*)(const PulseKmtQueryAdapterInfo*);
    using CloseFn = LONG(WINAPI*)(const PulseKmtCloseAdapter*);
    HMODULE module = nullptr;
    EnumFn enumerate = nullptr;
    QueryFn query = nullptr;
    CloseFn close = nullptr;
    PulseKmtApi() = default;
    PulseKmtApi(const PulseKmtApi&) = delete;
    PulseKmtApi& operator=(const PulseKmtApi&) = delete;
    ~PulseKmtApi() { Reset(); }
    bool Load() {
        if (module) return true;
        module = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
        enumerate = reinterpret_cast<EnumFn>(GetProcAddress(module, "D3DKMTEnumAdapters2"));
        query = reinterpret_cast<QueryFn>(GetProcAddress(module, "D3DKMTQueryAdapterInfo"));
        close = reinterpret_cast<CloseFn>(GetProcAddress(module, "D3DKMTCloseAdapter"));
        if (enumerate && query && close) return true;
        Reset();
        return false;
    }
    void Reset() {
        if (module) FreeLibrary(module);
        module = nullptr;
        enumerate = nullptr;
        query = nullptr;
        close = nullptr;
    }
};

void PulseKmtCloseAdapters(const PulseKmtApi& api, std::vector<PulseKmtAdapterInfo>& adapters) {
    for (auto& adapter : adapters) {
        if (adapter.hAdapter && api.close) {
            const PulseKmtCloseAdapter request{adapter.hAdapter};
            api.close(&request);
        }
        adapter.hAdapter = 0;
    }
    adapters.clear();
}

CRITICAL_SECTION g_print{};
struct TimingRow {
    const wchar_t* name = nullptr;
    unsigned long long count = 0;
    double wall = 0, cpu = 0, max_wall = 0, max_cpu = 0;
};
std::array<TimingRow, 26> g_timings{{
    {L"Battery"}, {L"Power scheme"}, {L"PDH Energy Meter"}, {L"PDH GPU Engine"},
    {L"Network throughput"}, {L"NIC discovery (within throughput)"}, {L"CPU"}, {L"Memory"},
    {L"Process scan"}, {L"Network view (no HTTP)"}, {L"Sample total (includes stages)"},
    {L"Network primary"}, {L"Network backups (parallel)"}, {L"Network callback"},
    {L"Memory detail"}, {L"PDH Disk IO"}, {L"System disk"}, {L"PDH CPU frequency"},
    {L"Uptime"}, {L"PDH GPU memory"}, {L"Wi-Fi"}, {L"Battery health"},
    {L"PDH CPU utility (shared query)"}, {L"FPS"},
    {L"D3DKMT GPU adapters"}, {L"D3DKMT GPU temperature"}
}};

void OnTiming(const wchar_t* name, double wall, double cpu, void*) {
    // No console I/O or allocation in the measured sampling/network paths.
    EnterCriticalSection(&g_print);
    for (auto& row : g_timings) {
        if (!row.name) row.name = name;
        if (std::wcscmp(row.name, name) != 0) continue;
        ++row.count;
        row.wall += wall; row.cpu += cpu;
        row.max_wall = std::max(row.max_wall, wall);
        row.max_cpu = std::max(row.max_cpu, cpu);
        break;
    }
    LeaveCriticalSection(&g_print);
}

unsigned long long ProcessCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

void PrintTimings(double wall_ms, double process_cpu_ms) {
    std::wprintf(L"\n每次实际采样耗时 ms / 线程 CPU ms（QPC / GetThreadTimes）\n");
    for (const auto& row : g_timings) {
        if (!row.count) {
            std::wprintf(L"%ls: n=0（未执行）\n", row.name);
            continue;
        }
        const double count = static_cast<double>(row.count);
        std::wprintf(L"%ls: n=%llu, 每次 %.4f ms / %.4f CPU ms, 最大 %.4f / %.4f, 累计 %.4f / %.4f\n",
            row.name, row.count, row.wall / count, row.cpu / count,
            row.max_wall, row.max_cpu, row.wall, row.cpu);
    }
    std::wprintf(L"总墙钟 %.1f ms，进程全部线程 CPU %.3f ms，单核占用 %.4f%%\n",
        wall_ms, process_cpu_ms, wall_ms > 0 ? 100 * process_cpu_ms / wall_ms : 0.0);
    std::wprintf(L"Sample total 包含各采样项；NIC discovery 包含于网速，不可相加。\n"
                 L"PDH CPU utility (shared query) 包含于 CPU；频率复用这次采集。\n"
                 L"Network primary/backups 是协调线程耗时；Network callback 单列。\n"
                 L"进程 CPU 包含 WinHTTP 内部线程、初始化、诊断及打印开销。线程 CPU 可能因计时粒度显示 0；建议 --brief --samples 120。\n");
}

bool CheckDurations() {
    const std::uint64_t seconds[] = {0, 59, 60, 3599, 3600, 3660, 90000};
    const wchar_t* expected[] = {L"0 秒", L"59 秒", L"1 分 0 秒", L"59 分 59 秒",
                                 L"1 小时 0 分", L"1 小时 1 分", L"25 小时 0 分"};
    for (std::size_t i = 0; i < sizeof(seconds) / sizeof(seconds[0]); ++i) {
        if (pulse::FormatOutageDuration(seconds[i]) != expected[i]) {
            std::wprintf(L"FAIL duration at %llu seconds\n", static_cast<unsigned long long>(seconds[i]));
            return false;
        }
    }
    std::wprintf(L"duration boundaries: PASS\n");
    return true;
}

const wchar_t* ModeName(pulse::PowerMode mode) {
    switch (mode) {
    case pulse::PowerMode::Unknown:
        return L"Unknown";
    case pulse::PowerMode::NoBattery:
        return L"NoBattery";
    case pulse::PowerMode::Discharging:
        return L"Discharging";
    case pulse::PowerMode::Charging:
        return L"Charging";
    case pulse::PowerMode::AcIdle:
        return L"AcIdle";
    }
    return L"?";
}

const wchar_t* NetName(pulse::NetState state) {
    switch (state) {
    case pulse::NetState::Checking:
        return L"Checking";
    case pulse::NetState::Online:
        return L"Online";
    case pulse::NetState::Slow:
        return L"Slow";
    case pulse::NetState::Offline:
        return L"Offline";
    }
    return L"?";
}

void LockPrint() { EnterCriticalSection(&g_print); }
void UnlockPrint() { LeaveCriticalSection(&g_print); }

void PrintDouble(const wchar_t* key, double value) {
    LockPrint();
    if (std::isnan(value)) std::wprintf(L"%ls=nan\n", key);
    else std::wprintf(L"%ls=%ls\n", key, std::to_wstring(value).c_str());
    UnlockPrint();
}

void PrintBool(const wchar_t* key, bool value) {
    LockPrint();
    std::wprintf(L"%ls=%ls\n", key, value ? L"true" : L"false");
    UnlockPrint();
}

void PrintInt(const wchar_t* key, long long value) {
    LockPrint();
    std::wprintf(L"%ls=%ls\n", key, std::to_wstring(value).c_str());
    UnlockPrint();
}

void PrintText(const wchar_t* key, const std::wstring& value) {
    LockPrint();
    std::wprintf(L"%ls=%ls\n", key, value.c_str());
    UnlockPrint();
}

void PrintSample(int index, const pulse::Snapshot& s) {
    LockPrint();
    std::wprintf(L"--- sample %ls ---\n", std::to_wstring(index).c_str());
    UnlockPrint();
    PrintText(L"mode", ModeName(s.mode));
    PrintInt(L"percent", s.percent);
    PrintDouble(L"power_w", s.power_w);
    PrintDouble(L"eta_s", s.eta_s);
    PrintBool(L"eta_from_system", s.eta_from_system);
    PrintDouble(L"eta20_s", s.eta20_s);
    PrintDouble(L"pct_per_hour", s.pct_per_hour);
    PrintDouble(L"on_battery_s", s.on_battery_s);
    PrintBool(L"on_battery_since_start", s.on_battery_since_start);
    PrintDouble(L"avg_w", s.avg_w);
    PrintDouble(L"remaining_wh", s.remaining_wh);
    PrintDouble(L"full_wh", s.full_wh);
    PrintBool(L"battery_saver", s.battery_saver);
    PrintText(L"power_scheme", s.power_scheme);
    PrintDouble(L"pkg_w", s.pkg_w);
    PrintDouble(L"cpu_w", s.cpu_w);
    PrintDouble(L"gpu_w", s.gpu_w);
    PrintDouble(L"mem_w", s.mem_w);
    PrintBool(L"pkg_power_from_energy", s.pkg_power_from_energy);
    PrintDouble(L"pkg_energy_delta_pwh", s.pkg_energy_delta_pwh);
    PrintDouble(L"pkg_energy_interval_s", s.pkg_energy_interval_s);
    PrintDouble(L"pkg_power_counter_w", s.pkg_power_counter_w);
    PrintDouble(L"gpu_percent", s.gpu_percent);
    PrintDouble(L"down_bps", s.down_bps);
    PrintDouble(L"up_bps", s.up_bps);
    PrintInt(L"rx_total", static_cast<long long>(s.rx_total));
    PrintInt(L"tx_total", static_cast<long long>(s.tx_total));
    PrintText(L"net_state", NetName(s.net_state));
    PrintDouble(L"latency_ms", s.latency_ms);
    PrintInt(L"offline_since_unix", static_cast<long long>(s.offline_since_unix));
    PrintBool(L"offline_by_latency", s.offline_by_latency);
    PrintDouble(L"cpu_percent", s.cpu_percent);
    PrintDouble(L"cpu_time_percent_GetSystemTimes", s.cpu_time_percent);
    PrintDouble(L"cpu_utility_percent_PDH_uncapped", s.cpu_utility_percent);
    PrintBool(L"cpu_from_utility", s.cpu_from_utility);
    PrintDouble(L"mem_percent", s.mem_percent);
    PrintInt(L"top_count", static_cast<long long>(s.top_procs.size()));
    for (std::size_t i = 0; i < s.top_procs.size(); ++i) {
        const std::wstring name_key = L"top" + std::to_wstring(i) + L"_name";
        const std::wstring cpu_key = L"top" + std::to_wstring(i) + L"_cpu";
        PrintText(name_key.c_str(), s.top_procs[i].name);
        PrintDouble(cpu_key.c_str(), s.top_procs[i].cpu_percent);
    }
    PrintDouble(L"self_cpu_percent", s.self_cpu_percent);
    PrintDouble(L"mem_used_gb", s.mem_used_gb);
    PrintDouble(L"mem_total_gb", s.mem_total_gb);
    PrintDouble(L"disk_read_bps", s.disk_read_bps);
    PrintDouble(L"disk_write_bps", s.disk_write_bps);
    PrintDouble(L"sys_disk_used_percent", s.sys_disk_used_percent);
    PrintDouble(L"sys_disk_free_gb", s.sys_disk_free_gb);
    PrintDouble(L"cpu_mhz", s.cpu_mhz);
    PrintInt(L"process_count", s.process_count);
    PrintDouble(L"uptime_s", s.uptime_s);
    PrintDouble(L"gpu_shared_mem_mb", s.gpu_shared_mem_mb);
    PrintDouble(L"gpu_dedicated_mem_mb", s.gpu_dedicated_mem_mb);
    PrintDouble(L"gpu_temp_c", s.gpu_temp_c);
    PrintText(L"wifi_ssid", s.wifi_ssid);
    PrintInt(L"wifi_signal", s.wifi_signal);
    PrintDouble(L"battery_health_percent", s.battery_health_percent);
    PrintInt(L"battery_cycles", s.battery_cycles);
    PrintDouble(L"fps", s.fps);
    PrintInt(L"fps_status", s.fps_status);
    PrintText(L"fps_process", s.fps_process);
    LockPrint();
    std::fflush(stdout);
    UnlockPrint();
}

// KMTQUERYADAPTERINFOTYPE in the same Windows SDK d3dkmthk.h: registry info
// is 8, not 2 (2 is UMOPENGLINFO). This is only used by --gpu, not sampling.
constexpr UINT kPulseKmtAdapterRegistryInfo = 8;
struct PulseKmtAdapterRegistryInfo {
    WCHAR AdapterString[MAX_PATH];
    WCHAR BiosString[MAX_PATH];
    WCHAR DacType[MAX_PATH];
    WCHAR ChipType[MAX_PATH];
};
static_assert(sizeof(void*) == 8, "x64 only");
static_assert(sizeof(PulseKmtAdapterRegistryInfo) == 2080, "D3DKMT_ADAPTERREGISTRYINFO size");
static_assert(offsetof(PulseKmtAdapterRegistryInfo, BiosString) == 520, "BiosString offset");
static_assert(offsetof(PulseKmtAdapterRegistryInfo, DacType) == 1040, "DacType offset");
static_assert(offsetof(PulseKmtAdapterRegistryInfo, ChipType) == 1560, "ChipType offset");

int RunGpuTest() {
    std::wprintf(L"GPU 诊断：gdi32 D3DKMT，每秒一轮，共 5 轮；不创建 D3D 设备，无需管理员。\n"
                 L"Type=15, RegistryInfo=8, PerfData=62; Temperature 单位 0.1°C；0 或查询失败表示不可用。\n"
                 L"SoftwareDevice mask=0x4, RenderSupported mask=0x1; Power 原始单位为 0.1%%。\n");
    PulseKmtApi api;
    if (!api.Load()) {
        std::wprintf(L"无法加载 gdi32.dll 的 D3DKMTEnumAdapters2 / QueryAdapterInfo / CloseAdapter。\n");
        return 1;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    int result = 0;
    for (int round = 1; round <= 5; ++round) {
        if (round > 1) Sleep(1000);
        // Diagnostic mode intentionally includes software/display-only adapters.
        // RAII also closes partial enumeration results on all error paths.
        struct Adapters {
            const PulseKmtApi& api;
            std::vector<PulseKmtAdapterInfo> rows;
            explicit Adapters(const PulseKmtApi& value) : api(value) {}
            ~Adapters() { PulseKmtCloseAdapters(api, rows); }
        } adapters(api);
        PulseKmtEnumAdapters2 request{};
        LONG status = api.enumerate(&request);
        std::wprintf(L"\nround=%d Enum(count) NTSTATUS=0x%08lX capacity=%lu\n", round,
                     static_cast<unsigned long>(status), request.NumAdapters);
        if (status < 0 || request.NumAdapters > 4096) {
            result = 1;
            continue;
        }
        if (request.NumAdapters == 0) {
            std::wprintf(L"  adapters=0\n");
            continue;
        }
        adapters.rows.assign(request.NumAdapters, PulseKmtAdapterInfo{});
        request.pAdapters = adapters.rows.data();
        status = api.enumerate(&request);
        std::wprintf(L"Enum(adapters) NTSTATUS=0x%08lX adapters=%lu\n",
                     static_cast<unsigned long>(status), request.NumAdapters);
        if (status < 0 || request.NumAdapters > adapters.rows.size()) {
            result = 1;
            continue;
        }
        for (ULONG i = 0; i < request.NumAdapters; ++i) {
            const auto& adapter = adapters.rows[i];
            PulseKmtAdapterType type{};
            const PulseKmtQueryAdapterInfo type_query{adapter.hAdapter, kPulseKmtAdapterType,
                &type, static_cast<UINT>(sizeof(type))};
            const LONG type_status = api.query(&type_query);
            PulseKmtAdapterRegistryInfo registry{};
            const PulseKmtQueryAdapterInfo registry_query{adapter.hAdapter, kPulseKmtAdapterRegistryInfo,
                &registry, static_cast<UINT>(sizeof(registry))};
            const LONG registry_status = api.query(&registry_query);
            registry.AdapterString[MAX_PATH - 1] = L'\0';
            PulseKmtAdapterPerfData data{};
            data.PhysicalAdapterIndex = 0;
            const PulseKmtQueryAdapterInfo perf_query{adapter.hAdapter, kPulseKmtAdapterPerfData,
                &data, static_cast<UINT>(sizeof(data))};
            LARGE_INTEGER before{}, after{};
            QueryPerformanceCounter(&before);
            const LONG perf_status = api.query(&perf_query);
            QueryPerformanceCounter(&after);
            const double elapsed = frequency.QuadPart > 0 ?
                (after.QuadPart - before.QuadPart) * 1000.0 / frequency.QuadPart : 0;
            const double celsius = perf_status >= 0 && data.Temperature > 0 && data.Temperature < 1500 ?
                data.Temperature / 10.0 : NAN;
            std::wprintf(L"  adapter=%lu LUID=%08lX:%08lX type=0x%08X software=%ls render=%ls name=%ls\n",
                i, static_cast<unsigned long>(adapter.AdapterLuid.HighPart), adapter.AdapterLuid.LowPart,
                type.Value, type_status < 0 ? L"unknown" : (type.Value & kPulseKmtSoftwareDevice) ? L"true" : L"false",
                type_status < 0 ? L"unknown" : (type.Value & kPulseKmtRenderSupported) ? L"true" : L"false",
                registry_status >= 0 ? registry.AdapterString : L"--");
            std::wprintf(L"    NTSTATUS: type=0x%08lX registry=0x%08lX perf=0x%08lX\n",
                static_cast<unsigned long>(type_status), static_cast<unsigned long>(registry_status),
                static_cast<unsigned long>(perf_status));
            std::wprintf(L"    Temperature=%lu (%.1f°C) FanRPM=%lu Power=%lu (%.1f%%) perf_ms=%.4f\n",
                data.Temperature, celsius, data.FanRPM, data.Power, data.Power / 10.0, elapsed);
        }
        std::fflush(stdout);
    }
    return result;
}

int RunFpsTest(const pulse::Settings& settings, bool full, bool no_probe) {
    std::wprintf(L"FPS 自测：先启动采集，再每秒打印一次，共 10 次。\n"
                 L"status：0 未启用，1 正常，2 需要管理员或 Performance Log Users，3 前台没有 Present，4 其他错误。\n"
                 L"前台若是本进程、桌面或任务栏，沿用上一个程序；本进程首次置前时选后方应用。请在这 10 秒内切到要测的窗口。\n"
                 L"帧率取前台进程及其子孙里、滞后窗口 [现在-2秒, 现在-1秒) 内 Present 最多的一个进程，不把多个进程相加。\n"
                 L"会话启动不足 2 秒时 fps 为 nan。top 的 frames 是同一窗口内的 Present 次数，用来确认事件是否到达。\n");
    pulse::Metrics metrics;
    pulse::Wants wants;
    wants.chip_power = false;
    wants.gpu_usage = false;
    wants.processes = false;
    wants.fps = true;
    metrics.SetWants(wants);
    metrics.SetProbeEnabled(!no_probe);
    metrics.Start(settings, nullptr, nullptr);
    std::wprintf(L"采样模式：%ls\n", full ? L"完整" : L"迷你");
    metrics.Sample(full);
    for (int i = 0; i < 10; ++i) {
        Sleep(1000);
        const pulse::Snapshot snapshot = metrics.Sample(full);
        if (std::isnan(snapshot.fps)) {
            std::wprintf(L"t=%d fps=nan status=%d process=%ls\n", i, snapshot.fps_status,
                         snapshot.fps_process.c_str());
        } else {
            std::wprintf(L"t=%d fps=%.1f status=%d process=%ls\n", i, snapshot.fps, snapshot.fps_status,
                         snapshot.fps_process.c_str());
        }
        if (snapshot.fps_top.empty()) {
            std::wprintf(L"  top=(none)\n");
        } else {
            for (std::size_t n = 0; n < snapshot.fps_top.size(); ++n) {
                const auto& rank = snapshot.fps_top[n];
                std::wprintf(L"  top%llu name=%ls pid=%lu frames=%llu\n",
                             static_cast<unsigned long long>(n + 1), rank.name.c_str(),
                             static_cast<unsigned long>(rank.pid),
                             static_cast<unsigned long long>(rank.frames));
            }
        }
        std::fflush(stdout);
    }
    return 0;
}

void OnNet(const pulse::NetEvent& event, void*) {
    LockPrint();
    std::wprintf(L"NET recovered=%ls msg=%ls\n", event.recovered ? L"true" : L"false",
                 event.message.c_str());
    std::fflush(stdout);
    UnlockPrint();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    int samples = 60, interval_ms = 1000, switch_every = 0;
    bool full = true, brief = false, fps_test = false, no_probe = false, count_only = false, gpu_test = false;
    pulse::Settings settings;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--compact") == 0) full = false;
        else if (std::wcscmp(argv[i], L"--brief") == 0) brief = true;
        else if (std::wcscmp(argv[i], L"--no-probe") == 0) no_probe = true;
        else if (std::wcscmp(argv[i], L"--process-count-only") == 0) count_only = true;
        else if (std::wcscmp(argv[i], L"--gpu") == 0) gpu_test = true;
        else if (std::wcscmp(argv[i], L"--url") == 0 && i + 1 < argc)
            settings.probe_urls.emplace_back(argv[++i]);
        else if (std::wcscmp(argv[i], L"--slow-ms") == 0 && i + 1 < argc) {
            wchar_t* end = nullptr;
            const long value = std::wcstol(argv[++i], &end, 10);
            if (!end || *end || (value != 300 && value != 500 && value != 800)) return 2;
            settings.slow_ms = static_cast<int>(value);
        }
        else if (std::wcscmp(argv[i], L"--samples") == 0 && i + 1 < argc) {
            wchar_t* end = nullptr;
            const long value = std::wcstol(argv[++i], &end, 10);
            if (!end || *end || value < 1 || value > 86400) return 2;
            samples = static_cast<int>(value);
        } else if (std::wcscmp(argv[i], L"--interval-ms") == 0 && i + 1 < argc) {
            wchar_t* end = nullptr;
            const long value = std::wcstol(argv[++i], &end, 10);
            if (!end || *end || (value != 500 && value != 1000 && value != 2000)) return 2;
            interval_ms = static_cast<int>(value);
        } else if (std::wcscmp(argv[i], L"--fps") == 0) fps_test = true;
        else if (std::wcscmp(argv[i], L"--switch-every") == 0 && i + 1 < argc) {
            wchar_t* end = nullptr;
            const long value = std::wcstol(argv[++i], &end, 10);
            if (!end || *end || value < 1 || value > 86400) return 2;
            switch_every = static_cast<int>(value);
        } else {
            std::wprintf(L"Usage: metrics_selftest.exe [--samples N] [--compact] [--brief] [--interval-ms 500|1000|2000] [--switch-every N] [--slow-ms 300|500|800] [--url URL ...] [--fps] [--gpu] [--no-probe] [--process-count-only]\n");
            return 2;
        }
    }
    if (gpu_test) return RunGpuTest();
    if (!CheckDurations()) return 1;
    if (fps_test) return RunFpsTest(settings, full, no_probe);
    InitializeCriticalSection(&g_print);
    pulse::diagnostics::SetTimingCallback(OnTiming, nullptr);
    LARGE_INTEGER frequency{}, start{}, finish{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const auto cpu_start = ProcessCpuTicks();
    {
        pulse::Metrics metrics;
        const auto wants_for = [count_only](bool detailed, pulse::PowerMode mode) {
            pulse::Wants w;
            w.chip_power = detailed || mode != pulse::PowerMode::Discharging;
            w.gpu_usage = w.processes = detailed;
            w.memory_detail = w.disk_io = w.sys_disk = w.cpu_freq = detailed;
            w.uptime = w.gpu_memory = w.wifi = w.battery_health = detailed;
            w.power_scheme = w.process_count = detailed;
            w.gpu_temp = detailed;
            if (count_only) {
                w.chip_power = w.gpu_usage = w.processes = false;
                w.memory_detail = w.disk_io = w.sys_disk = w.cpu_freq = false;
                w.uptime = w.gpu_memory = w.wifi = w.battery_health = w.power_scheme = false;
                w.process_count = true;
                w.gpu_temp = false;
            }
            return w;
        };
        pulse::PowerMode mode = pulse::PowerMode::Unknown;
        metrics.SetWants(wants_for(full, mode));
        std::wprintf(L"mode=%ls samples=%d full_interval_ms=%d compact_interval_ms=2000 switch_every=%d log=%ls\n",
            full ? L"full" : L"compact", samples, interval_ms, switch_every, metrics.LogPath().c_str());
        std::wprintf(L"Energy raw: pWh; W = delta_pwh * 3.6e-9 / QPC_seconds. Compact/fallback: Power mW / 1000.\n");
        std::wprintf(L"CPU: Processor Utility (PDH, display clamped 0..100); GetSystemTimes prints the old Processor Time basis.\n"
                     L"cpu_from_utility=false means warmup/fallback; unknown first interval prints nan. Network slow_ms=%d.\n", settings.slow_ms);
        metrics.SetProbeEnabled(!no_probe);
        metrics.Start(settings, OnNet, nullptr);
        for (int i = 0; i < samples; ++i) {
            const bool detailed = switch_every && (i / switch_every) % 2 ? !full : full;
            if (i > 0) Sleep(detailed ? static_cast<DWORD>(interval_ms) : 2000u);
            metrics.SetWants(wants_for(detailed, mode));
            const auto snapshot = metrics.Sample(detailed);
            mode = snapshot.mode;
            if (!brief) {
                std::wprintf(L"collection_mode=%ls\n", detailed ? L"full" : L"compact");
                PrintSample(i, snapshot);
            }
        }
    } // Stop WinHTTP and wait for its callbacks before clearing the diagnostics sink.
    const auto cpu_finish = ProcessCpuTicks();
    QueryPerformanceCounter(&finish);
    pulse::diagnostics::SetTimingCallback(nullptr, nullptr);
    const double wall_ms = frequency.QuadPart > 0 ?
        (finish.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart : 0.0;
    PrintTimings(wall_ms, static_cast<double>(cpu_finish - cpu_start) / 10000.0);
    DeleteCriticalSection(&g_print);
    return 0;
}

