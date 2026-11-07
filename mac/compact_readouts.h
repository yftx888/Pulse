#pragma once
#include "../src/metrics.h"
#include "../src/ui_model.h"
#include <cmath>
#include <cwchar>
#include <string>

namespace pulse::mac {
struct CompactReadouts {
    std::wstring watts = L"--W", remaining = L"--:--";
    bool operator==(const CompactReadouts& other) const {
        return watts == other.watts && remaining == other.remaining;
    }
};

// Use the existing whole-machine sources and ETA estimate. Package power and
// time-to-full are deliberately ineligible for these unlabelled readouts.
inline CompactReadouts MakeCompactReadouts(const Snapshot& s) {
    CompactReadouts result;
    const bool whole = s.power_trace == PowerTrace::BatteryWhole;
    const bool onBattery = whole && s.mode == PowerMode::Discharging &&
        s.ac_known && !s.ac_online && !s.power_contested && !s.battery_supplemental;
    double watts = NAN;
    if (onBattery && s.battery_rate_valid && std::isfinite(s.power_w) && s.power_w > 0)
        watts = s.power_w;
    else if (!whole && s.power_trace != PowerTrace::Unknown && s.ac_known && s.ac_online)
        watts = s.system_w;
    if (std::isfinite(watts) && watts >= 0 && watts < 1.0e6)
        result.watts = ui::Number(watts, 1) + L"W";

    if (onBattery && (s.battery_rate_valid || s.eta_from_system) &&
        std::isfinite(s.eta_s) && s.eta_s >= 0 && s.eta_s < 1.0e9) {
        const auto minutes = static_cast<unsigned long long>(s.eta_s / 60);
        wchar_t text[32]{};
        std::swprintf(text, 32, L"%llu:%02llu", minutes / 60, minutes % 60);
        result.remaining = text;
    }
    return result;
}
} // namespace pulse::mac
