#pragma once
#include "../src/metrics.h"
#include <array>
#include <string>

namespace pulse { namespace mac {
// Same-thread profiler: install before Sample, remove before destroying its user data.
using TimingCallback = void (*)(const char* stage, double milliseconds, void* user);
void SetTimingCallback(TimingCallback callback, void* user);
std::wstring BuildNetworkDiagnosticsReport(const Settings& settings = Settings{});
std::wstring BuildProcessDiagnosticsReport();
std::wstring BuildGpuDiagnosticsReport();
double SelfCpuSeconds();
// Same sampling thread; empty on AC or when no battery-side rate is available.
const wchar_t* LastBatteryPowerSource();
const wchar_t* LastSystemPowerSource();
// Compact mode has no package-watt consumer other than the power component.
// Both menu bar and selftest use the preceding actual sample, never stale trace state.
inline bool CanSkipCompactPackagePower(const Snapshot& previous) {
    return previous.power_trace == PowerTrace::Package && std::isfinite(previous.system_w) &&
        previous.system_w >= 0 && previous.system_w < 1.0e6;
}
// Streaming diagnostics: callback receives each timestamped line/block immediately.
void RunTelemetryDiagnostics(int samples, void (*output)(const std::wstring&));
void RunMemoryDiagnostics(void (*output)(const std::wstring&));
} }
