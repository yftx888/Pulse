// Small, side-effect-free interpretation shared by PDH sampling and diagnostics.
#pragma once
#include "power_state.h"
#include <string>

namespace pulse {

// PDH_CSTATUS_VALID_DATA and PDH_CSTATUS_NEW_DATA are 0 and 1 respectively.
inline bool MeterDataValid(std::uint32_t status) { return status == 0 || status == 1; }
inline bool MeterPowerValid(double mw) {
    return std::isfinite(mw) && mw >= 0.0 && mw < 1.0e9;
}

struct MeterEvidence {
    bool counter = false;
    bool array_ok = false;
    bool matched = false; // Match by name, including an instance with bad CStatus.
};

inline MeterStatus ResolveMeterStatus(const MeterEvidence& energy, const MeterEvidence& power,
                                     double watts, bool warming) {
    if (std::isfinite(watts) && watts >= 0.0 && watts < 1.0e6) return MeterStatus::Valid;
    if (warming) return MeterStatus::Warmup;
    if (!energy.counter && !power.counter) return MeterStatus::CounterUnavailable;
    if ((energy.counter && (!energy.array_ok || energy.matched)) ||
        (power.counter && (!power.array_ok || power.matched))) return MeterStatus::ReadFailed;
    // Absence is asserted only after all available arrays were read successfully.
    return MeterStatus::NoMatch;
}

struct MeterEnergySample {
    std::wstring name;
    std::uint32_t status = 0;
    std::uint64_t pwh = 0;
};

enum class MeterInterval { Unavailable, InvalidData, Warmup, Reset, Unchanged, Advanced };
struct MeterDomainResult {
    MeterStatus status = MeterStatus::CounterUnavailable;
    MeterInterval interval = MeterInterval::Unavailable;
    std::uint64_t delta_pwh = 0;
    double energy_w = NAN;
    double power_w = NAN;
};

inline MeterDomainResult InterpretMeterDomain(const MeterEvidence& energy, const MeterEvidence& power,
    const MeterEnergySample* before, const MeterEnergySample* after, double seconds,
    std::uint32_t power_status, double power_mw) {
    MeterDomainResult out;
    bool warming = false;
    if (energy.array_ok && after) {
        if (!MeterDataValid(after->status) || (before && !MeterDataValid(before->status))) {
            out.interval = MeterInterval::InvalidData;
        } else if (!before || before->name != after->name || !(seconds > 0.0) || !std::isfinite(seconds)) {
            out.interval = MeterInterval::Warmup;
            warming = true;
        } else if (after->pwh < before->pwh) {
            out.interval = MeterInterval::Reset;
            warming = true;
        } else {
            out.delta_pwh = after->pwh - before->pwh;
            out.interval = out.delta_pwh ? MeterInterval::Advanced : MeterInterval::Unchanged;
            // A stationary provider is not evidence of 0 W in a manual diagnostic.
            warming = !out.delta_pwh;
            const double watts = static_cast<double>(out.delta_pwh) * 3.6e-9 / seconds;
            if (out.delta_pwh && std::isfinite(watts) && watts < 1.0e6) out.energy_w = watts;
        }
    }
    if (power.array_ok && power.matched && MeterDataValid(power_status) && MeterPowerValid(power_mw))
        out.power_w = power_mw / 1000.0;
    const double watts = std::isfinite(out.energy_w) ? out.energy_w : out.power_w;
    out.status = ResolveMeterStatus(energy, power, watts, warming);
    return out;
}

} // namespace pulse
