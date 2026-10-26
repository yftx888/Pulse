// Shared battery-source classification for the sampler, the UI, and tests.
// Keep this header free of Win32 calls so tests can use the same rules.
#pragma once
#include <cmath>
#include <cstdint>

namespace pulse {

enum class PowerMode { Unknown, NoBattery, Discharging, Charging, AcIdle };

// What the mini power cell and the power chart are allowed to draw.
// BatteryWhole is the only case that may label battery power as the whole machine.
enum class PowerTrace { Unknown, Package, BatteryWhole, BatterySide };

// Energy Meter domain state. A failure or warmup is the current sample only;
// callers must replace it on the next attempt instead of latching "no hardware".
enum class MeterStatus : std::uint8_t {
    Off = 0,
    Warmup,
    CounterUnavailable,
    NoMatch,
    ReadFailed,
    Valid
};

// Rates at or beyond this magnitude are treated as unknown, not as 0 W.
// The historical sampler used the same guard (BATTERY_UNKNOWN_RATE is far outside it).
inline constexpr long kBatteryRateUnknownAbs = 500000;
inline constexpr double kAcDischargeMinMw = 1000.0;

inline bool BatteryRateKnown(long rate) {
    return rate > -kBatteryRateUnknownAbs && rate < kBatteryRateUnknownAbs;
}

struct BatterySample {
    bool read_ok = false;
    bool present = false;
    bool ac_known = false;
    bool ac_online = false;
    bool charging = false;
    bool discharging = false;
    bool rate_known = false;
    double rate_mw = 0; // Signed. Ignored unless rate_known. Positive charges, negative discharges.
};

struct BatteryClassification {
    PowerMode mode = PowerMode::Unknown;
    bool ac_known = false;
    bool ac_online = false;
    bool rate_valid = false;
    double rate_mw = NAN; // Signed. NAN when the rate is not valid.
    bool contested = false;
    bool supplemental = false; // AC is online and valid battery discharge is at least 1 W.
    bool pure_battery = false; // AC is known offline and discharge is not contested.
    bool show_watts = false;
    bool eta_discharge = false;
    bool eta_charge = false;
    PowerTrace trace = PowerTrace::Unknown;
};

// Conflict policy:
// - A non-zero valid rate owns the sign: positive charges, negative discharges.
// - Both direction flags, or one flag that contradicts that sign, are contested.
// - Contested positive rate is not reported as charging and its watts stay unknown.
// - Contested negative rate still describes battery discharge, but it is not
//   pure-battery "whole machine" power and it does not feed ETA or unplug averages.
// - A valid zero rate is idle, not 0 W of charge or discharge, even if one flag is set.
// - With no valid rate, a single flag can set the direction; derived watts/ETA stay unknown.
//   A confirmed pure-battery session may still use Windows' separate life-time estimate.
// - Pure battery requires a successful read, a known offline AC line, discharge, and no conflict.
// - AC online requires a valid discharge rate of at least 1 W; otherwise it is AC idle.
//   Supplemental discharge is never whole-machine power and cannot feed ETA or unplug averages.
//   The sampler confirms it for 10 s and holds it for 5 s through brief rate drops.
// - Unknown AC cannot be labeled whole-machine; discharge then uses the battery-side trace.
inline BatteryClassification ClassifyBattery(const BatterySample& in) {
    BatteryClassification out;
    if (!in.read_ok) {
        out.rate_mw = NAN;
        out.trace = PowerTrace::Unknown;
        out.mode = PowerMode::Unknown;
        return out;
    }
    out.ac_known = in.ac_known;
    out.ac_online = in.ac_known && in.ac_online;
    if (!in.present) {
        out.mode = PowerMode::NoBattery;
        out.trace = PowerTrace::Package;
        out.rate_mw = NAN;
        return out;
    }

    const bool rate_known = in.rate_known && std::isfinite(in.rate_mw) &&
        in.rate_mw > static_cast<double>(-kBatteryRateUnknownAbs) &&
        in.rate_mw < static_cast<double>(kBatteryRateUnknownAbs);
    out.rate_valid = rate_known;
    out.rate_mw = rate_known ? in.rate_mw : NAN;
    const bool both_flags = in.charging && in.discharging;
    const bool rate_charge = rate_known && in.rate_mw > 0.0;
    const bool rate_discharge = rate_known && in.rate_mw < 0.0;
    const bool nonzero = rate_charge || rate_discharge;
    bool contradicts = false;
    if (nonzero) {
        if (both_flags) contradicts = true;
        else if (rate_discharge && in.charging && !in.discharging) contradicts = true;
        else if (rate_charge && in.discharging && !in.charging) contradicts = true;
    }
    out.contested = both_flags || contradicts;

    enum class Flow { Unknown, Idle, Charge, Discharge };
    Flow flow = Flow::Unknown;
    if (rate_known && in.rate_mw == 0.0 && !both_flags) {
        flow = Flow::Idle;
    } else if (out.contested) {
        flow = rate_discharge ? Flow::Discharge : Flow::Unknown;
    } else if (rate_discharge) {
        flow = Flow::Discharge;
    } else if (rate_charge) {
        flow = Flow::Charge;
    } else if (!rate_known && in.discharging && !in.charging) {
        flow = Flow::Discharge;
    } else if (!rate_known && in.charging && !in.discharging) {
        flow = Flow::Charge;
    } else if (rate_known && in.rate_mw == 0.0) {
        flow = Flow::Idle;
    }

    const bool ac_off = out.ac_known && !out.ac_online;
    const bool ac_on = out.ac_known && out.ac_online;
    if (ac_on && flow == Flow::Discharge &&
        (!rate_known || std::fabs(in.rate_mw) < kAcDischargeMinMw)) flow = Flow::Idle;
    out.pure_battery = ac_off && flow == Flow::Discharge && !out.contested;
    out.supplemental = ac_on && flow == Flow::Discharge;
    out.show_watts = nonzero && (flow == Flow::Charge || flow == Flow::Discharge) &&
        (!out.contested || rate_discharge);
    if (!rate_known) out.show_watts = false;
    out.eta_discharge = out.pure_battery && out.show_watts;
    out.eta_charge = flow == Flow::Charge && out.show_watts && !out.contested;

    if (flow == Flow::Discharge) out.mode = PowerMode::Discharging;
    else if (flow == Flow::Charge) out.mode = PowerMode::Charging;
    else if (flow == Flow::Idle || (flow == Flow::Unknown && ac_on)) out.mode = PowerMode::AcIdle;
    else out.mode = PowerMode::Unknown;

    if (out.pure_battery) out.trace = PowerTrace::BatteryWhole;
    else if (flow == Flow::Discharge && !ac_on) out.trace = PowerTrace::BatterySide;
    else out.trace = PowerTrace::Package;
    return out;
}

// Uses elapsed time, not sample counts. A separate pending flag permits time zero.
struct AcDischargeDebounce {
    bool Update(bool raw, bool hard_exit, unsigned long long now_ms) {
        if (hard_exit) {
            Reset();
            return false;
        }
        if (raw == confirmed_) {
            pending_ = false;
            return confirmed_;
        }
        if (!pending_ || now_ms < pending_since_) {
            pending_ = true;
            pending_since_ = now_ms;
        }
        const unsigned long long delay = confirmed_ ? 5000ULL : 10000ULL;
        if (now_ms - pending_since_ >= delay) {
            confirmed_ = raw;
            pending_ = false;
        }
        return confirmed_;
    }

    void Reset() {
        confirmed_ = false;
        pending_ = false;
        pending_since_ = 0;
    }

private:
    bool confirmed_ = false;
    bool pending_ = false;
    unsigned long long pending_since_ = 0;
};

// Package power is skipped only for a confirmed battery-side trace.
// Unknown and supplemental discharge still need the processor counter.
inline bool NeedsPackagePower(PowerTrace trace) {
    return trace != PowerTrace::BatteryWhole && trace != PowerTrace::BatterySide;
}

// Next collection-source cache. `previous` is the trace already driving Wants.
// `observed` is this sample's Snapshot.power_trace and is not modified.
// Display, chart points, and the unplug integral keep using that Snapshot;
// a failed read remains Unknown there and is not replaced by a stale trace.
//
// Unknown must not change a confirmed collection source. BatteryWhole and
// BatterySide keep package counters released, and Package keeps them open
// through a hybrid session. The cache starts Unknown, so sampling still
// follows NeedsPackagePower(Unknown) until a source is confirmed; that is
// how no-battery and AC are recognized. A later confirmed Package,
// BatteryWhole, or BatterySide replaces the cache immediately, including
// the return to package power when hybrid discharge is observed.
inline PowerTrace NextCollectionTrace(PowerTrace previous, PowerTrace observed) {
    if (observed == PowerTrace::Unknown && previous != PowerTrace::Unknown)
        return previous;
    return observed;
}

inline bool UseSystemBatteryEta(const BatteryClassification& battery, double derived, double system) {
    return battery.pure_battery && !(derived >= 0.0) && std::isfinite(system) && system >= 0.0;
}

}  // namespace pulse
