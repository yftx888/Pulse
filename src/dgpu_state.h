// Pure dGPU gate and power interpretation shared by sampling and diagnostics.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulse {

enum class DgpuStatus : std::uint8_t {
    Off = 0, NoNvidia, Sleeping, NvmlUnavailable, NoMatch, Unsupported, ReadFailed, Valid, Glitch
};

inline constexpr int kDgpuNvmlSuccess = 0;
inline constexpr int kDgpuNvmlNotSupported = 3;
inline constexpr unsigned kDgpuPowerD0 = 1; // DEVICE_POWER_STATE, winnt.h.

struct DgpuAdapter {
    unsigned vendor = 0, bus = 0, device = 0, power_state = 0;
    bool power_read_ok = true;
};
struct DgpuDevice {
    unsigned bus = 0, device = 0;
    int power_result = -1;
    unsigned power_mw = 0, limit_mw = 0;
    bool limit_known = false;
};
struct DgpuResult {
    DgpuStatus status = DgpuStatus::Off;
    double watts = NAN;
    std::size_t matched = 0;
};

enum class DgpuAction : std::uint8_t { None, Init, Read };
inline DgpuAction DgpuNextAction(bool gate_awake, bool session_open,
                                 unsigned long long now, unsigned long long retry_at) {
    if (!gate_awake) return DgpuAction::None;
    if (session_open) return DgpuAction::Read;
    return now < retry_at ? DgpuAction::None : DgpuAction::Init;
}
inline bool DgpuAwake(const DgpuAdapter& adapter) {
    return adapter.vendor == 0x10DE && adapter.power_read_ok && adapter.power_state == kDgpuPowerD0;
}
inline bool DgpuMatches(const std::vector<DgpuAdapter>& adapters, unsigned bus, unsigned device) {
    for (const auto& adapter : adapters)
        if (DgpuAwake(adapter) && adapter.bus == bus && adapter.device == device) return true;
    return false;
}
inline DgpuStatus DgpuGateStatus(const std::vector<DgpuAdapter>& adapters) {
    bool nvidia = false;
    for (const auto& adapter : adapters) {
        if (DgpuAwake(adapter)) return DgpuStatus::Valid; // Gate open; power still needs a valid read.
        nvidia = nvidia || adapter.vendor == 0x10DE;
    }
    return nvidia ? DgpuStatus::Sleeping : DgpuStatus::NoNvidia;
}
inline bool DgpuPowerGlitch(unsigned power_mw, unsigned limit_mw, bool limit_known) {
    if (!power_mw) return true;
    // Multiply in 64 bits to avoid overflow and preserve the exact 1.25 boundary.
    return limit_known ? std::uint64_t(power_mw) * 4 > std::uint64_t(limit_mw) * 5 : power_mw >= 1000000;
}
inline DgpuResult InterpretDgpu(const std::vector<DgpuAdapter>& adapters,
                                const std::vector<DgpuDevice>& devices) {
    DgpuResult result;
    result.status = DgpuGateStatus(adapters);
    if (result.status != DgpuStatus::Valid) return result;
    double watts = 0;
    bool unsupported = false, failed = false, glitch = false;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        if (!DgpuMatches(adapters, device.bus, device.device)) continue;
        // Duplicate logical adapters/devices must not count one board twice.
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j)
            if (devices[j].bus == device.bus && devices[j].device == device.device) duplicate = true;
        if (duplicate) continue;
        ++result.matched;
        if (device.power_result == kDgpuNvmlNotSupported) unsupported = true;
        else if (device.power_result != kDgpuNvmlSuccess) failed = true;
        else if (DgpuPowerGlitch(device.power_mw, device.limit_mw, device.limit_known)) glitch = true;
        else watts += device.power_mw / 1000.0;
    }
    if (!result.matched) result.status = DgpuStatus::NoMatch;
    else if (failed) result.status = DgpuStatus::ReadFailed;
    else if (unsupported) result.status = DgpuStatus::Unsupported;
    else if (glitch) result.status = DgpuStatus::Glitch;
    else { result.status = DgpuStatus::Valid; result.watts = watts; }
    return result;
}
struct DgpuHistory {
    DgpuResult last;
    unsigned long long valid_at = 0;
    void Reset() { last = {}; valid_at = 0; }
    DgpuResult Apply(DgpuResult result, unsigned long long now) {
        if (result.status == DgpuStatus::Valid) { last = result; valid_at = now; }
        else if (result.status == DgpuStatus::Glitch && last.status == DgpuStatus::Valid &&
                 result.matched == last.matched && now >= valid_at && now - valid_at < 5000) {
            result.status = DgpuStatus::Valid;
            result.watts = last.watts; // A substituted value never extends the grace period.
        }
        return result;
    }
};
inline const wchar_t* DgpuStatusName(DgpuStatus status) {
    switch (status) {
    case DgpuStatus::Off: return L"Off";
    case DgpuStatus::NoNvidia: return L"NoNvidia";
    case DgpuStatus::Sleeping: return L"Sleeping";
    case DgpuStatus::NvmlUnavailable: return L"NvmlUnavailable";
    case DgpuStatus::NoMatch: return L"NoMatch";
    case DgpuStatus::Unsupported: return L"Unsupported";
    case DgpuStatus::ReadFailed: return L"ReadFailed";
    case DgpuStatus::Valid: return L"Valid";
    case DgpuStatus::Glitch: return L"Glitch";
    }
    return L"Unknown";
}

} // namespace pulse
