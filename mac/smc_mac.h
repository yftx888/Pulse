#pragma once
#include <string>

namespace pulse { namespace mac {
// Diagnostics only: no Metrics/Snapshot changes, subprocesses, or data files.
void RunSmcDiagnostics(int samples, void (*output)(const std::wstring&));
namespace detail {
// The bridge in metrics_mac.mm owns the existing BatteryRegistry for this run.
using SmcBatteryReader = std::wstring (*)(void*);
void RunSmcDiagnosticsWithBattery(int samples, void (*output)(const std::wstring&),
                                  SmcBatteryReader read_battery, void* battery);
}
} }
