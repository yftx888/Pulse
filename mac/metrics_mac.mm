#include "metrics_mac.h"
#include "mac_strings.h"
#include "netprobe_mac.h"
#include "smc_mac.h"
#include "smc_reader.h"
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/processor_info.h>
#include <mach/vm_statistics.h>
#include <mach/vm_page_size.h>
#include <libproc.h>
#include <sys/resource.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <unistd.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iomanip>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace pulse { namespace mac {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
thread_local TimingCallback timing_callback = nullptr;
thread_local void* timing_user = nullptr;
struct Timing {
    const char* name;
    Clock::time_point start = Clock::now();
    explicit Timing(const char* n) : name(n) {}
    ~Timing() {
        if (timing_callback) timing_callback(name,
            std::chrono::duration<double, std::milli>(Clock::now() - start).count(), timing_user);
    }
};
template<class T> class OwnedCF {
public:
    explicit OwnedCF(T value = nullptr) : value_(value) {}
    ~OwnedCF() { if (value_) CFRelease(value_); }
    OwnedCF(const OwnedCF&) = delete;
    OwnedCF& operator=(const OwnedCF&) = delete;
    T get() const { return value_; }
    void reset(T v = nullptr) { if (value_) CFRelease(value_); value_ = v; }
private:
    T value_;
};
class OwnedIO {
public:
    explicit OwnedIO(io_object_t value = IO_OBJECT_NULL) : value_(value) {}
    ~OwnedIO() { if (value_) IOObjectRelease(value_); }
    OwnedIO(const OwnedIO&) = delete;
    OwnedIO& operator=(const OwnedIO&) = delete;
    io_object_t get() const { return value_; }
private:
    io_object_t value_;
};
struct Timebase {
    mach_timebase_info_data_t info{};
    Timebase() { if (mach_timebase_info(&info) != KERN_SUCCESS || !info.denom) info = {1, 1}; }
    double Seconds(std::uint64_t ticks) const { return static_cast<double>(ticks) * info.numer / info.denom * 1e-9; }
};
const Timebase& MachTime() { static const Timebase value; return value; }
std::uint64_t NowMs() { return static_cast<std::uint64_t>(MachTime().Seconds(mach_absolute_time()) * 1000.0); }
int Cores() { static const int value = static_cast<int>(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN))); return value; }
CFTypeRef Get(CFDictionaryRef dict, CFStringRef key) {
    return dict && CFGetTypeID(dict) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(dict, key) : nullptr;
}
double Number(CFTypeRef value) {
    double result = NAN;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &result)) return NAN;
    return std::isfinite(result) ? result : NAN;
}
double FindNumber(CFDictionaryRef dict, std::initializer_list<CFStringRef> keys) {
    for (CFStringRef key : keys) { const double n = Number(Get(dict, key)); if (std::isfinite(n)) return n; }
    return NAN;
}
bool Boolean(CFTypeRef value, bool& result) {
    if (!value) return false;
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) { result = CFBooleanGetValue(static_cast<CFBooleanRef>(value)); return true; }
    const double number = Number(value);
    if (number == 0 || number == 1) { result = number != 0; return true; }
    return false;
}
bool FindBoolean(CFDictionaryRef dict, std::initializer_list<CFStringRef> keys, bool& result) {
    for (auto key : keys) if (Boolean(Get(dict, key), result)) return true;
    return false;
}
std::wstring Describe(CFTypeRef value) {
    if (!value) return L"--";
    OwnedCF<CFStringRef> description(CFCopyDescription(value));
    return FromCF(description.get());
}
// NS/CF descriptions recursively expand dictionaries and arrays, including PowerTelemetryData.
void DumpDictionary(std::wostream& out, CFDictionaryRef dict) {
    if (!dict) { out << L"--\n"; return; }
    const CFIndex count = CFDictionaryGetCount(dict);
    std::vector<const void*> keys(static_cast<std::size_t>(count)), values(keys.size());
    CFDictionaryGetKeysAndValues(dict, keys.data(), values.data());
    for (CFIndex i = 0; i < count; ++i) out << Describe(keys[static_cast<std::size_t>(i)])
        << L" = " << Describe(values[static_cast<std::size_t>(i)]) << L"\n";
}
CFMutableDictionaryRef BatteryProperties(bool* query_ok = nullptr) {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSmartBattery");
    if (!matching) { if (query_ok) *query_ok = false; return nullptr; }
    io_iterator_t iterator = IO_OBJECT_NULL;
    const kern_return_t status = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
    if (query_ok) *query_ok = status == KERN_SUCCESS;
    if (status != KERN_SUCCESS) return nullptr;
    OwnedIO it(iterator);
    OwnedIO service(IOIteratorNext(iterator));
    if (!service.get()) return nullptr;
    CFMutableDictionaryRef props = nullptr;
    const bool ok = IORegistryEntryCreateCFProperties(service.get(), &props, kCFAllocatorDefault, 0) == KERN_SUCCESS;
    if (query_ok) *query_ok = ok;
    if (!ok) { if (props) CFRelease(props); return nullptr; }
    return props;
}

// A service is retained between samples; missing optional keys are not read errors.
class BatteryRegistry {
    io_service_t service_ = IO_OBJECT_NULL;
public:
    ~BatteryRegistry() { Reset(); }
    BatteryRegistry() = default;
    BatteryRegistry(const BatteryRegistry&) = delete;
    BatteryRegistry& operator=(const BatteryRegistry&) = delete;
    void Reset() { if (service_) IOObjectRelease(service_); service_ = IO_OBJECT_NULL; }
    bool Ensure(bool* query_ok = nullptr) {
        if (service_) { if (query_ok) *query_ok = true; return true; }
        CFMutableDictionaryRef matching = IOServiceMatching("AppleSmartBattery");
        if (!matching) { if (query_ok) *query_ok = false; return false; }
        io_iterator_t iterator = IO_OBJECT_NULL;
        const auto status = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
        OwnedIO it(iterator);
        if (query_ok) *query_ok = status == KERN_SUCCESS;
        if (status == KERN_SUCCESS) service_ = IOIteratorNext(iterator);
        return service_ != IO_OBJECT_NULL;
    }
    CFTypeRef Copy(CFStringRef key) const {
        return service_ ? IORegistryEntryCreateCFProperty(service_, key, kCFAllocatorDefault, 0) : nullptr;
    }
    CFMutableDictionaryRef Read(const Wants& wants, bool& query_ok) {
        Timing timing("Battery registry");
        if (!Ensure(&query_ok)) return nullptr;
        OwnedCF<CFMutableDictionaryRef> props(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        if (!props.get()) { query_ok = false; return nullptr; }
        bool any = false;
        const auto read = [&](CFStringRef key) {
            OwnedCF<CFTypeRef> value(Copy(key));
            if (!value.get()) return false;
            CFDictionarySetValue(props.get(), key, value.get());
            any = true; return true;
        };
        const bool voltage_read = read(CFSTR("Voltage"));
        if (!read(CFSTR("InstantAmperage"))) read(CFSTR("Amperage"));
        for (auto key : {CFSTR("AppleRawCurrentCapacity"), CFSTR("AppleRawMaxCapacity"),
             CFSTR("ExternalConnected"), CFSTR("IsCharging"), CFSTR("BatteryInstalled")}) read(key);
        if (wants.battery_health) { read(CFSTR("DesignCapacity")); read(CFSTR("CycleCount")); }
        // CreateCFProperty has no status output. Missing Voltage (normally present) is
        // our stale-service signal. Keep any partial data and use IOPS as before.
        if (!voltage_read) { Reset(); query_ok = false; }
        if (!any) return nullptr;
        CFRetain(props.get()); return props.get();
    }
};

// IOPS descriptions are borrowed from the info object. Keep only parsed values
// between samples, never CF references whose owner has already been released.
struct BatteryIopsCache {
    bool valid = false, sources_available = false, found = false;
    bool present_known = false, present = false, ac_known = false, ac_online = false;
    bool charging = false;
    double capacity = NAN, maximum = NAN, time_to_empty = NAN, time_to_full = NAN;
    bool registry_external_known = false, registry_external = false;
    bool registry_charging_known = false, registry_charging = false;
    Clock::time_point at{};
    void Reset() { valid = false; }
    void Read(CFDictionaryRef props, bool registry_ok) {
        bool external = false, is_charging = false;
        const bool external_known = FindBoolean(props, {CFSTR("ExternalConnected")}, external);
        const bool charging_known = FindBoolean(props, {CFSTR("IsCharging")}, is_charging);
        const auto now = Clock::now();
        if (valid && registry_ok && now - at < std::chrono::seconds(10) &&
            external_known == registry_external_known && (!external_known || external == registry_external) &&
            charging_known == registry_charging_known && (!charging_known || is_charging == registry_charging)) return;
        Timing timing("Battery IOPS");
        sources_available = found = present_known = present = ac_known = ac_online = charging = false;
        capacity = maximum = time_to_empty = time_to_full = NAN;
        OwnedCF<CFTypeRef> info(IOPSCopyPowerSourcesInfo());
        OwnedCF<CFArrayRef> sources(info.get() ? IOPSCopyPowerSourcesList(info.get()) : nullptr);
        sources_available = sources.get() != nullptr;
        if (sources.get()) for (CFIndex i = 0; i < CFArrayGetCount(sources.get()); ++i) {
            CFDictionaryRef source = IOPSGetPowerSourceDescription(info.get(), CFArrayGetValueAtIndex(sources.get(), i));
            CFTypeRef type = Get(source, CFSTR(kIOPSTypeKey));
            if (!type || !CFEqual(type, CFSTR(kIOPSInternalBatteryType))) continue;
            found = true;
            present_known = Boolean(Get(source, CFSTR(kIOPSIsPresentKey)), present);
            CFTypeRef state = Get(source, CFSTR(kIOPSPowerSourceStateKey));
            if (state && (CFEqual(state, CFSTR(kIOPSACPowerValue)) || CFEqual(state, CFSTR(kIOPSBatteryPowerValue)))) {
                ac_known = true; ac_online = CFEqual(state, CFSTR(kIOPSACPowerValue));
            }
            Boolean(Get(source, CFSTR(kIOPSIsChargingKey)), charging);
            capacity = Number(Get(source, CFSTR(kIOPSCurrentCapacityKey)));
            maximum = Number(Get(source, CFSTR(kIOPSMaxCapacityKey)));
            time_to_empty = Number(Get(source, CFSTR(kIOPSTimeToEmptyKey)));
            time_to_full = Number(Get(source, CFSTR(kIOPSTimeToFullChargeKey)));
            break;
        }
        // Failed IOPS reads are retried next sample rather than cached as success.
        valid = sources_available;
        at = Clock::now();
        registry_external_known = external_known; registry_external = external;
        registry_charging_known = charging_known; registry_charging = is_charging;
    }
};

// Ported from src/metrics.cpp, Metrics::Impl::UpdateBattery (session_changed
// through on_battery_s/avg_w). Energy is Wh here, rate magnitude remains mW.
struct BatteryStatistics {
    bool have_ema = false, prev_valid = false, prev_ok = false;
    bool prev_ac_known = false, prev_ac_online = false, prev_contested = false, prev_pure = false;
    bool since_start = false;
    PowerMode prev_mode = PowerMode::Unknown;
    double ema = 0, energy_wh = 0, measured_s = 0;
    std::uint64_t unplug_tick = 0;
    Clock::time_point previous{};
    // Wake/pause/mode changes discard only dt; never integrate a sleep gap.
    void BreakInterval() { previous = {}; }
    void Failed() { have_ema = false; ema = 0; prev_ok = false; previous = {}; }
    void Update(Snapshot& s, const BatteryClassification& classified, double system_eta) {
        constexpr double kTauSec = 30, kMaxGapSec = 5, kAvgMinSec = 60;
        const auto at = Clock::now();
        const bool has_dt = previous != Clock::time_point{};
        const double dt = has_dt ? std::chrono::duration<double>(at - previous).count() : 0;
        previous = at;
        const auto now = NowMs();
        const bool session_changed = !prev_valid || !prev_ok || prev_mode != classified.mode ||
            prev_ac_known != classified.ac_known || prev_ac_online != classified.ac_online ||
            prev_contested != classified.contested || prev_pure != classified.pure_battery;
        const double magnitude = classified.show_watts ? std::fabs(classified.rate_mw) : 0;
        if (classified.show_watts && (session_changed || !have_ema || !has_dt || !(dt > 0))) {
            ema = magnitude; have_ema = true;
        } else if (classified.show_watts) {
            const double alpha = 1 - std::exp(-dt / kTauSec);
            ema += alpha * (magnitude - ema);
        } else { have_ema = false; ema = 0; }
        if (classified.pure_battery) {
            if (!prev_pure) {
                unplug_tick = now; since_start = !prev_valid; energy_wh = measured_s = 0;
            } else if (prev_ok && has_dt && dt > 0 && dt <= kMaxGapSec && classified.show_watts) {
                energy_wh += (magnitude / 1000) * dt / 3600;
                measured_s += dt;
            }
        } else unplug_tick = 0;
        double eta = NAN, eta20 = NAN, pph = NAN;
        if (have_ema && ema > 0 && s.mode != PowerMode::NoBattery) {
            const double remaining_mwh = s.remaining_wh * 1000, full_mwh = s.full_wh * 1000;
            if (classified.eta_discharge) {
                eta = remaining_mwh / ema * 3600;
                if (full_mwh > 0) {
                    pph = ema / full_mwh * 100;
                    if (s.percent > 20) {
                        const double remain = (remaining_mwh - .2 * full_mwh) / ema * 3600;
                        if (remain >= 0) eta20 = remain;
                    }
                }
            } else if (classified.eta_charge && full_mwh > remaining_mwh) {
                eta = (full_mwh - remaining_mwh) / ema * 3600;
                if (full_mwh > 0) pph = ema / full_mwh * 100;
            }
        }
        if (UseSystemBatteryEta(classified, eta, system_eta)) { eta = system_eta; s.eta_from_system = true; }
        if (classified.pure_battery && s.percent >= 0 && s.percent <= 20) eta20 = 0;
        s.eta_s = eta; s.eta20_s = eta20; s.pct_per_hour = pph;
        if (classified.pure_battery && unplug_tick != 0) {
            s.on_battery_s = static_cast<double>(now - unplug_tick) / 1000;
            s.on_battery_since_start = since_start;
            if (s.on_battery_s > 0 && measured_s > kAvgMinSec) s.avg_w = energy_wh / (measured_s / 3600);
        }
        prev_mode = classified.mode; prev_ac_known = classified.ac_known; prev_ac_online = classified.ac_online;
        prev_contested = classified.contested; prev_pure = classified.pure_battery; prev_ok = prev_valid = true;
    }
};
thread_local const wchar_t* last_battery_source = L"";
thread_local const wchar_t* last_system_source = L"";
struct BatterySmc {
    SmcReader reader;
    bool opened = false;
    Clock::time_point open_retry_after{};
    std::map<std::uint32_t, Clock::time_point> key_retry_after;
    void Close() {
        reader.Close(); opened = false; key_retry_after.clear(); open_retry_after = {};
    }
    bool Read(std::uint32_t key, double maximum, double& watts) {
        const auto now = Clock::now();
        if (!opened) {
            if (now < open_retry_after) return false;
            std::wostringstream ignored;
            opened = reader.Open(ignored);
            if (!opened) {
                reader.Close(); open_retry_after = now + std::chrono::seconds(60); return false;
            }
        }
        const auto retry = key_retry_after.find(key);
        if (retry != key_retry_after.end() && now < retry->second) return false;
        // Info is cached: each successful steady-state read issues just Bytes=5.
        // Optional-key/value failures retain the shared connection; a failed
        // driver call invalidates it and retries opening after the same backoff.
        kern_return_t kernel = KERN_SUCCESS;
        if (!reader.ReadFloat(key, watts, &kernel) || watts < 0 || watts >= maximum) {
            if (kernel != KERN_SUCCESS) {
                Close(); open_retry_after = now + std::chrono::seconds(60);
            } else {
                reader.ForgetInfo(key);
                key_retry_after[key] = now + std::chrono::seconds(60);
            }
            return false;
        }
        key_retry_after.erase(key);
        return true;
    }
    // Signed milliwatts. |value| >= 300000 is a bad sample and uses the same
    // per-key backoff as an out-of-range float. The caller applies sign rules.
    bool ReadMilliWatts(std::uint32_t key, std::int32_t& milliWatts) {
        constexpr std::int32_t kLimitMw = 300000;
        const auto now = Clock::now();
        if (!opened) {
            if (now < open_retry_after) return false;
            std::wostringstream ignored;
            opened = reader.Open(ignored);
            if (!opened) {
                reader.Close(); open_retry_after = now + std::chrono::seconds(60); return false;
            }
        }
        const auto retry = key_retry_after.find(key);
        if (retry != key_retry_after.end() && now < retry->second) return false;
        // Same connection policy as Read: a failed driver call closes it;
        // a bad value only backs off this key.
        kern_return_t kernel = KERN_SUCCESS;
        std::int32_t raw = 0;
        if (!reader.ReadInt32LE(key, raw, &kernel) || raw <= -kLimitMw || raw >= kLimitMw) {
            if (kernel != KERN_SUCCESS) {
                Close(); open_retry_after = now + std::chrono::seconds(60);
            } else {
                reader.ForgetInfo(key);
                key_retry_after[key] = now + std::chrono::seconds(60);
            }
            return false;
        }
        key_retry_after.erase(key);
        milliWatts = raw;
        return true;
    }
};

void ReadBattery(Snapshot& s, const Wants& wants, AcDischargeDebounce& debounce,
                 BatteryRegistry& registry, BatteryIopsCache& iops, BatterySmc& smc, BatteryStatistics& stats) {
    last_battery_source = last_system_source = L"";
    Timing timing("Battery");
    s.battery_saver = NSProcessInfo.processInfo.lowPowerModeEnabled;
    bool registry_ok = false;
    OwnedCF<CFMutableDictionaryRef> props(registry.Read(wants, registry_ok));
    iops.Read(props.get(), registry_ok);
    BatterySample input;
    input.read_ok = iops.found || props.get() || (registry_ok && iops.sources_available);
    input.present = iops.found || props.get();
    bool present = input.present;
    if (iops.present_known && !iops.present) input.present = false;
    if (props.get() && FindBoolean(props.get(), {CFSTR("BatteryInstalled")}, present)) input.present = present;
    if (iops.ac_known) { input.ac_known = true; input.ac_online = iops.ac_online; }
    // Registry is the more direct external-power flag when available.
    bool external = false;
    if (FindBoolean(props.get(), {CFSTR("ExternalConnected")}, external)) {
        input.ac_known = true; input.ac_online = external;
    }
    bool charging = false;
    if (!FindBoolean(props.get(), {CFSTR("IsCharging")}, charging))
        charging = iops.charging;
    input.charging = charging;
    // AC-off is evidence of discharging; don't manufacture a flag from the rate sign.
    input.discharging = input.ac_known && !input.ac_online && !charging;
    const double voltage = FindNumber(props.get(), {CFSTR("Voltage")});
    double current = FindNumber(props.get(), {CFSTR("InstantAmperage"), CFSTR("Amperage")});
    if (current > 2147483647.0 && current <= 4294967295.0) current -= 4294967296.0;
    if (voltage > 0 && voltage < 100000 && std::isfinite(current) && std::fabs(current) < 100000) {
        input.rate_mw = voltage * current / 1000.0;
        input.rate_known = std::isfinite(input.rate_mw) && std::fabs(input.rate_mw) < kBatteryRateUnknownAbs;
    }
    // B0AP is signed mW (charge > 0, discharge < 0) and updates about once a second.
    // On battery a positive value is rejected and PPBR is the fallback. On AC, PPBR
    // is not read; PSTR below is the separate whole-machine sample.
    bool used_smc_rate = false;
    if (input.read_ok && input.present && input.ac_known) {
        std::int32_t milli = 0;
        if (smc.ReadMilliWatts(Code("B0AP"), milli) && (input.ac_online || milli <= 0)) {
            input.rate_mw = static_cast<double>(milli);
            input.rate_known = true;
            last_battery_source = L"smc-b0ap";
            used_smc_rate = true;
        } else if (!input.ac_online) {
            double watts = NAN;
            if (smc.Read(Code("PPBR"), 300, watts)) {
                input.rate_mw = -watts * 1000;
                input.rate_known = true;
                last_battery_source = L"smc-ppbr";
                used_smc_rate = true;
            }
        }
    }
    // V×I remains the selected rate when SMC did not win, including when AC is unknown.
    if (!used_smc_rate && input.rate_known) last_battery_source = L"registry";
    // PSTR measures machine load, independently of the battery charge direction.
    if (input.ac_known && input.ac_online) {
        double watts = NAN;
        if (smc.Read(Code("PSTR"), 500, watts)) {
            s.system_w = watts; last_system_source = L"smc";
        }
    }
    auto classified = ClassifyBattery(input);
    const bool raw = classified.supplemental;
    // Enter needs 10s and exit needs 5s; a positive rate hard-exits immediately.
    // A one-second negative B0AP sample therefore cannot confirm AC discharge.
    // Rates under 1 W are already AcIdle before this debounce runs.
    const bool confirmed = debounce.Update(raw,
        !input.read_ok || !input.present || !classified.ac_known || !classified.ac_online ||
        charging || (classified.rate_valid && classified.rate_mw > 0), NowMs());
    if (confirmed) {
        classified.mode = PowerMode::Discharging; classified.supplemental = true;
        classified.pure_battery = false; classified.show_watts = classified.rate_valid && classified.rate_mw < 0;
        classified.eta_discharge = classified.eta_charge = false; classified.trace = PowerTrace::Package;
    } else if (raw) {
        classified.mode = PowerMode::AcIdle; classified.supplemental = false;
        classified.show_watts = false; classified.trace = PowerTrace::Package;
    }
    s.mode = classified.mode; s.power_trace = classified.trace;
    s.ac_known = classified.ac_known; s.ac_online = classified.ac_online;
    s.battery_rate_valid = classified.rate_valid; s.battery_rate_mw = classified.rate_mw;
    s.battery_supplemental = classified.supplemental; s.power_contested = classified.contested;
    s.power_w = classified.show_watts ? std::fabs(classified.rate_mw) / 1000.0 : NAN;
    if (!input.read_ok) { stats.Failed(); return; }
    if (!input.present) { stats.Update(s, classified, NAN); return; }
    const double capacity = iops.capacity;
    const double maximum = iops.maximum;
    if (capacity >= 0 && maximum > 0 && capacity <= maximum)
        s.percent = static_cast<int>(std::lround(capacity / maximum * 100.0));
    const double raw_current = FindNumber(props.get(), {CFSTR("AppleRawCurrentCapacity")});
    const double raw_max = FindNumber(props.get(), {CFSTR("AppleRawMaxCapacity")});
    if (voltage > 0 && voltage < 100000) {
        if (raw_current >= 0) s.remaining_wh = raw_current * voltage / 1000000.0;
        if (raw_max > 0) s.full_wh = raw_max * voltage / 1000000.0;
    }
    double minutes = NAN;
    if (classified.pure_battery) minutes = iops.time_to_empty;
    else if (classified.mode == PowerMode::Charging && !classified.contested)
        minutes = iops.time_to_full;
    stats.Update(s, classified, minutes >= 0 && std::isfinite(minutes) ? minutes * 60.0 : NAN);
    if (wants.battery_health) {
        const double design = FindNumber(props.get(), {CFSTR("DesignCapacity")});
        if (raw_max > 0 && design > 0) s.battery_health_percent = raw_max / design * 100.0;
        const double cycles = FindNumber(props.get(), {CFSTR("CycleCount")});
        if (cycles >= 0 && cycles < 1000000) s.battery_cycles = static_cast<int>(cycles);
    }
}

struct CpuCollector {
    std::vector<std::array<std::uint32_t, CPU_STATE_MAX>> previous;
    void Read(Snapshot& s) {
        Timing timing("CPU");
        s.cpu_percent = NAN;
        natural_t count = 0;
        processor_info_array_t data = nullptr;
        mach_msg_type_number_t length = 0;
        const host_t host = mach_host_self();
        const auto status = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &count, &data, &length);
        mach_port_deallocate(mach_task_self(), host);
        if (status != KERN_SUCCESS) { previous.clear(); return; }
        const auto release = [&] { vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(data),
            static_cast<vm_size_t>(length) * sizeof(integer_t)); };
        if (length < count * CPU_STATE_MAX) { release(); previous.clear(); return; }
        std::vector<std::array<std::uint32_t, CPU_STATE_MAX>> current(count);
        std::uint64_t total = 0, active = 0;
        const bool ready = previous.size() == current.size() && !current.empty();
        for (std::size_t i = 0; i < current.size(); ++i) for (int j = 0; j < CPU_STATE_MAX; ++j) {
            current[i][j] = static_cast<std::uint32_t>(data[i * CPU_STATE_MAX + j]);
            if (ready) {
                const std::uint32_t delta = current[i][j] - previous[i][j]; // Tick rollover is modulo 2^32.
                total += delta; if (j != CPU_STATE_IDLE) active += delta;
            }
        }
        release(); previous.swap(current);
        if (ready && total) s.cpu_percent = 100.0 * static_cast<double>(active) / total;
    }
};
void ReadMemory(Snapshot& s, bool detail) {
    Timing timing("Memory");
    s.mem_percent = NAN;
    std::uint64_t total = 0; std::size_t size = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0 || !total) return;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const host_t host = mach_host_self();
    vm_size_t page = 0;
    const bool ok = host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (!ok) return;
    // Phase 0.3: Activity Monitor used memory; free_count includes speculative.
    const auto free = vm.free_count >= vm.speculative_count ? vm.free_count - vm.speculative_count : 0;
    const double used = std::clamp(static_cast<double>(total) -
        (static_cast<double>(free) + vm.external_page_count) * page, 0.0, static_cast<double>(total));
    s.mem_percent = std::clamp(100.0 * used / total, 0.0, 100.0);
    if (detail) { s.mem_used_gb = used / kGiB; s.mem_total_gb = static_cast<double>(total) / kGiB; }
}

bool ListPids(std::vector<pid_t>& pids) {
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) return false;
    std::size_t capacity = static_cast<std::size_t>(bytes) / sizeof(pid_t) + 128;
    for (int attempt = 0; attempt < 4 && capacity < 2000000; ++attempt, capacity *= 2) {
        pids.resize(capacity);
        const int actual = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        if (actual <= 0) { pids.clear(); return false; }
        if (static_cast<std::size_t>(actual) >= pids.size() * sizeof(pid_t)) continue;
        pids.resize(static_cast<std::size_t>(actual) / sizeof(pid_t));
        // pid 0 is kernel_task, not an empty slot.
        pids.erase(std::remove_if(pids.begin(), pids.end(), [](pid_t pid) { return pid < 0; }), pids.end());
        std::sort(pids.begin(), pids.end()); pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
        return true;
    }
    pids.clear(); return false;
}
// ri_user_time/ri_system_time units are disputed (Mach ticks per XNU, nanoseconds per
// some user-space tools). Calibrate once against getrusage for this same process.
struct CpuTimeUnit { double seconds = NAN; double measured = NAN; bool mach = true; };
const CpuTimeUnit& ProcessCpuUnit() {
    static const CpuTimeUnit unit = [] {
        CpuTimeUnit result;
        const double tick = MachTime().Seconds(1);
        result.seconds = tick;
        // Short busy wait so the ratio is not dominated by microsecond rounding.
        const auto deadline = Clock::now() + std::chrono::milliseconds(200);
        volatile std::uint64_t spin = 0;
        while (SelfCpuSeconds() < 0.05 && Clock::now() < deadline)
            for (int i = 0; i < 100000; ++i) spin = spin + static_cast<std::uint64_t>(i);
        rusage_info_v2 own{};
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&own)) != 0) return result;
        const double self = SelfCpuSeconds();
        const double raw = static_cast<double>(own.ri_user_time + own.ri_system_time);
        if (!(raw > 0) || !(self > 0)) return result;
        result.measured = self / raw;
        // The candidates differ by about 41x on Apple Silicon; pick the closer one on a log scale.
        result.mach = std::fabs(std::log(result.measured / tick)) <= std::fabs(std::log(result.measured / 1e-9));
        result.seconds = result.mach ? tick : 1e-9;
        return result;
    }();
    return unit;
}
struct ProcessCollector {
    struct Point { std::uint64_t cpu, identity, at; };
    struct Ranked { pid_t pid; double percent; };
    std::map<pid_t, Point> previous;
    std::vector<Ranked> ranked;
    int listed = -1, unreadable = 0, denied = 0, gone_or_other = 0;
    bool Read(bool scan) {
        Timing timing("Process scan");
        std::vector<pid_t> pids;
        ranked.clear(); unreadable = denied = gone_or_other = 0;
        if (!ListPids(pids)) { listed = -1; previous.clear(); return false; }
        listed = static_cast<int>(pids.size());
        if (!scan) { previous.clear(); return true; }
        std::map<pid_t, Point> current;
        for (pid_t pid : pids) {
            if (pid == getpid()) continue;
            rusage_info_v2 usage{};
            errno = 0;
            if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
                ++unreadable; if (errno == EPERM || errno == EACCES) ++denied; else ++gone_or_other;
                continue;
            }
            const Point point{usage.ri_user_time + usage.ri_system_time, usage.ri_proc_start_abstime, mach_absolute_time()};
            current.emplace(pid, point);
            const auto old = previous.find(pid);
            if (old == previous.end() || old->second.identity != point.identity ||
                point.cpu < old->second.cpu || point.at <= old->second.at) continue;
            const double elapsed = MachTime().Seconds(point.at - old->second.at);
            const double cpu_s = static_cast<double>(point.cpu - old->second.cpu) * ProcessCpuUnit().seconds;
            if (elapsed > 0) ranked.push_back({pid, std::clamp(100.0 * cpu_s / elapsed / Cores(), 0.0, 100.0)});
        }
        previous.swap(current);
        std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
            return a.percent == b.percent ? a.pid < b.pid : a.percent > b.percent;
        });
        return true;
    }
    static std::wstring Name(pid_t pid) {
        char name[1024]{};
        const int n = proc_name(pid, name, static_cast<std::uint32_t>(sizeof(name)));
        return n > 0 ? FromUtf8(name) : std::to_wstring(pid);
    }
};

struct Nic { unsigned index; std::string name; unsigned type, flags; std::uint64_t rx, tx; bool include; std::wstring reason; };
bool ReadNics(std::vector<Nic>& rows, int& error, std::vector<unsigned char>& buffer) {
    int mib[] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
    std::size_t size = 0;
    bool ok = false, resize = buffer.empty();
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (resize) {
            if (sysctl(mib, 6, nullptr, &size, nullptr, 0) != 0) { error = errno; return false; }
            if (!size || size > 64 * 1024 * 1024) { error = EINVAL; return false; }
            // Leave headroom for an interface appearing between the two sysctls.
            buffer.resize(std::min<std::size_t>(size + 4096, 64 * 1024 * 1024));
        }
        size = buffer.size();
        if (sysctl(mib, 6, buffer.data(), &size, nullptr, 0) == 0) { ok = true; break; }
        if (errno != ENOMEM) { error = errno; return false; }
        resize = true;
    }
    if (!ok) { error = errno; return false; }
    rows.clear();
    for (std::size_t offset = 0; offset + 4 <= size;) {
        std::uint16_t length = 0; std::memcpy(&length, buffer.data() + offset, sizeof(length));
        const unsigned type = buffer[offset + 3];
        if (length < 4 || length > size - offset) { error = EINVAL; return false; }
        if (type == RTM_IFINFO2 && length >= sizeof(if_msghdr2)) {
            if_msghdr2 header{}; std::memcpy(&header, buffer.data() + offset, sizeof(header));
            char name[IF_NAMESIZE]{};
            // NET_RT_IFLIST2 puts the interface sockaddr immediately after this
            // header. Copy its fixed prefix so no alignment of sysctl bytes is assumed.
            constexpr std::size_t name_offset = offsetof(sockaddr_dl, sdl_data);
            const std::size_t address_bytes = length - sizeof(header);
            if ((header.ifm_addrs & RTA_IFP) && address_bytes >= name_offset) {
                const unsigned char* address = buffer.data() + offset + sizeof(header);
                sockaddr_dl link{}; std::memcpy(&link, address, name_offset);
                if (link.sdl_family == AF_LINK && link.sdl_len >= name_offset &&
                    link.sdl_len <= address_bytes && link.sdl_nlen > 0 &&
                    link.sdl_nlen < sizeof(name) && link.sdl_nlen <= link.sdl_len - name_offset &&
                    !std::memchr(address + name_offset, '\0', link.sdl_nlen))
                    std::memcpy(name, address + name_offset, link.sdl_nlen);
            }
            if (!name[0] && !if_indextoname(header.ifm_index, name)) { offset += length; continue; }
            Nic row{header.ifm_index, name, header.ifm_data.ifi_type, static_cast<unsigned>(header.ifm_flags),
                header.ifm_data.ifi_ibytes, header.ifm_data.ifi_obytes, true, L"计入"};
            if (row.flags & IFF_LOOPBACK) { row.include = false; row.reason = L"回环"; }
            else if (row.type != IFT_ETHER) { row.include = false; row.reason = L"非 IFT_ETHER"; }
            else if ((row.flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) {
                row.include = false; row.reason = L"未同时 UP/RUNNING";
            }
            for (const char* prefix : {"awdl", "llw", "anpi", "ap", "bridge", "utun"}) {
                if (row.name.compare(0, std::strlen(prefix), prefix) == 0) {
                    row.include = false; row.reason = L"排除前缀 " + FromUtf8(prefix); break;
                }
            }
            rows.push_back(std::move(row));
        }
        offset += length;
    }
    error = 0; return true;
}
struct NetworkCollector {
    struct Point { std::uint64_t rx, tx; std::string name; };
    std::map<unsigned, Point> previous;
    std::vector<unsigned char> buffer;
    std::uint64_t at = 0, total_rx = 0, total_tx = 0;
    void Reset() { previous.clear(); at = 0; }
    void Read(Snapshot& s) {
        Timing timing("Network throughput");
        std::vector<Nic> rows; int error = 0;
        s.down_bps = s.up_bps = NAN;
        s.rx_total = total_rx; s.tx_total = total_tx;
        if (!ReadNics(rows, error, buffer)) { Reset(); return; }
        const auto now = mach_absolute_time();
        const double elapsed = at && now > at ? MachTime().Seconds(now - at) : 0;
        std::uint64_t rx = 0, tx = 0;
        std::map<unsigned, Point> current;
        for (const Nic& nic : rows) if (nic.include) {
            current[nic.index] = {nic.rx, nic.tx, nic.name};
            const auto old = previous.find(nic.index);
            if (old == previous.end() || old->second.name != nic.name) continue;
            if (nic.rx >= old->second.rx) rx += nic.rx - old->second.rx;
            if (nic.tx >= old->second.tx) tx += nic.tx - old->second.tx;
        }
        previous.swap(current); at = now;
        total_rx += rx; total_tx += tx;
        s.rx_total = total_rx; s.tx_total = total_tx;
        if (elapsed > 0) { s.down_bps = static_cast<double>(rx) / elapsed; s.up_bps = static_cast<double>(tx) / elapsed; }
    }
};

double ReadGpu(std::wostream* out = nullptr) {
    Timing timing("GPU");
    io_iterator_t iterator = IO_OBJECT_NULL;
    auto matching = IOServiceMatching("IOAccelerator");
    if (!matching || IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) {
        if (out) *out << L"IOAccelerator 查询失败\n";
        return NAN;
    }
    OwnedIO it(iterator);
    double highest = NAN; int found = 0;
    while (io_object_t object = IOIteratorNext(iterator)) {
        OwnedIO service(object); ++found;
        io_name_t name{}; IORegistryEntryGetName(service.get(), name);
        OwnedCF<CFTypeRef> property(IORegistryEntryCreateCFProperty(service.get(), CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0));
        if (out) *out << L"IOAccelerator " << FromUtf8(name) << L"\n";
        if (!property.get() || CFGetTypeID(property.get()) != CFDictionaryGetTypeID()) {
            if (out) {
                *out << L"PerformanceStatistics 不存在或不是字典；实际服务全部属性：\n";
                CFMutableDictionaryRef actual = nullptr;
                IORegistryEntryCreateCFProperties(service.get(), &actual, kCFAllocatorDefault, 0);
                OwnedCF<CFMutableDictionaryRef> properties(actual);
                DumpDictionary(*out, properties.get());
            }
            continue;
        }
        const auto dict = static_cast<CFDictionaryRef>(property.get());
        if (out) DumpDictionary(*out, dict);
        const double percent = FindNumber(dict, {CFSTR("Device Utilization %"), CFSTR("GPU Utilization %")});
        if (percent >= 0 && percent <= 100) highest = std::isfinite(highest) ? std::max(highest, percent) : percent;
    }
    if (out) *out << L"服务数量=" << found << L"；候选 Device Utilization % / GPU Utilization %，结果=" << highest << L"\n";
    return highest;
}

struct DiskCollector {
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> previous;
    std::uint64_t at = 0;
    void Read(Snapshot& s) {
        Timing timing("Disk IO");
        io_iterator_t iterator = IO_OBJECT_NULL;
        auto matching = IOServiceMatching("IOBlockStorageDriver");
        if (!matching || IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) {
            previous.clear(); at = 0; return;
        }
        OwnedIO it(iterator);
        decltype(previous) current;
        std::uint64_t read_delta = 0, write_delta = 0;
        bool failed = false;
        while (io_object_t object = IOIteratorNext(iterator)) {
            OwnedIO service(object);
            std::uint64_t identity = 0;
            if (IORegistryEntryGetRegistryEntryID(object, &identity) != KERN_SUCCESS) { failed = true; continue; }
            OwnedCF<CFTypeRef> value(IORegistryEntryCreateCFProperty(object, CFSTR("Statistics"), kCFAllocatorDefault, 0));
            if (!value.get() || CFGetTypeID(value.get()) != CFDictionaryGetTypeID()) { failed = true; continue; }
            const auto stats = static_cast<CFDictionaryRef>(value.get());
            const double read = FindNumber(stats, {CFSTR("Bytes (Read)")});
            const double write = FindNumber(stats, {CFSTR("Bytes (Write)")});
            if (!(read >= 0 && write >= 0) || read >= 18446744073709551616.0 || write >= 18446744073709551616.0) { failed = true; continue; }
            const auto counters = std::make_pair(static_cast<std::uint64_t>(read), static_cast<std::uint64_t>(write));
            current[identity] = counters;
            const auto old = previous.find(identity);
            if (old == previous.end()) continue;
            if (counters.first >= old->second.first) read_delta += counters.first - old->second.first;
            if (counters.second >= old->second.second) write_delta += counters.second - old->second.second;
        }
        const auto now = mach_absolute_time();
        const double elapsed = at && now > at ? MachTime().Seconds(now - at) : 0;
        if (!failed && !current.empty() && elapsed > 0 && !previous.empty()) {
            s.disk_read_bps = static_cast<double>(read_delta) / elapsed;
            s.disk_write_bps = static_cast<double>(write_delta) / elapsed;
        }
        previous.swap(current); at = failed ? 0 : now;
    }
};
void ReadSystemDisk(Snapshot& s) {
    Timing timing("System disk");
    struct statfs stats{};
    if (statfs("/System/Volumes/Data", &stats) != 0 || !stats.f_blocks || !stats.f_bsize) return;
    s.sys_disk_used_percent = std::clamp(100.0 * (1.0 - static_cast<double>(stats.f_bfree) / stats.f_blocks), 0.0, 100.0);
    s.sys_disk_free_gb = static_cast<double>(stats.f_bavail) * stats.f_bsize / kGiB;
}
void ReadUptime(Snapshot& s) {
    int mib[] = {CTL_KERN, KERN_BOOTTIME}; timeval boot{}, now{}; std::size_t size = sizeof(boot);
    if (sysctl(mib, 2, &boot, &size, nullptr, 0) == 0 && gettimeofday(&now, nullptr) == 0)
        s.uptime_s = std::max(0.0, static_cast<double>(now.tv_sec - boot.tv_sec) + (now.tv_usec - boot.tv_usec) * 1e-6);
}
} // anonymous namespace

void SetTimingCallback(TimingCallback callback, void* user) { timing_callback = callback; timing_user = user; }
double SelfCpuSeconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return NAN;
    return static_cast<double>(usage.ru_utime.tv_sec) + usage.ru_utime.tv_usec * 1e-6 +
        static_cast<double>(usage.ru_stime.tv_sec) + usage.ru_stime.tv_usec * 1e-6;
}
namespace {
struct TelemetryInteger {
    bool valid = false;
    std::int64_t value = 0;
};
TelemetryInteger ParseTelemetryInteger(CFTypeRef value) {
    TelemetryInteger result;
    if (!value) return result;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        result.valid = CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result.value);
    } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
        const auto text = Utf8(FromCF(static_cast<CFStringRef>(value)));
        if (text.empty()) return result;
        errno = 0; char* end = nullptr;
        const long long n = std::strtoll(text.c_str(), &end, 10);
        if (!errno && end != text.c_str() && !*end) { result.valid = true; result.value = static_cast<std::int64_t>(n); }
    } else if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        result.valid = true; result.value = CFBooleanGetValue(static_cast<CFBooleanRef>(value)) ? 1 : 0;
    }
    return result;
}
void PrintInteger(std::wostream& out, const TelemetryInteger& value) {
    if (value.valid) out << value.value; else out << L"--";
}
} // anonymous namespace
void RunTelemetryDiagnostics(int samples, void (*output)(const std::wstring&)) {
    if (!output) return;
    samples = std::clamp(samples, 5, 600);
    const std::array<CFStringRef, 8> instant = {{CFSTR("SystemLoad"), CFSTR("SystemPowerIn"), CFSTR("BatteryPower"),
        CFSTR("AdapterEfficiencyLoss"), CFSTR("SystemVoltageIn"), CFSTR("SystemCurrentIn"),
        CFSTR("WallEnergyEstimate"), CFSTR("SystemEnergyConsumed")}};
    const std::array<CFStringRef, 3> accumulated = {{CFSTR("AccumulatedSystemLoad"), CFSTR("AccumulatedSystemPowerIn"), CFSTR("AccumulatedBatteryPower")}};
    const std::array<CFStringRef, 3> counts = {{CFSTR("SystemLoadAccumulatorCount"), CFSTR("SystemPowerInAccumulatorCount"), CFSTR("BatteryPowerAccumulatorCount")}};
    std::array<TelemetryInteger, 8> previous{};
    std::array<TelemetryInteger, 3> old_accumulated{}, old_counts{};
    std::array<unsigned, 8> changes{}, valid_pairs{};
    BatteryRegistry registry;
    const auto start = Clock::now();
    output(L"telemetry: 每秒读取一个 PowerTelemetryData 字典，另逐键读取 ExternalConnected/IsCharging；read_ms 仅计字典读取，不含首次匹配。\n"
           L"原始整数不经过 double；缺键/无有效相邻值/计数不递增时为 --；avg_mW 是待确认的累加差/计数差，不进 Snapshot。\n");
    for (int i = 0; i < samples; ++i) {
        std::this_thread::sleep_until(start + std::chrono::seconds(i));
        @autoreleasepool {
            const bool matched = registry.Ensure();
            const auto before = Clock::now();
            OwnedCF<CFTypeRef> value(registry.Copy(CFSTR("PowerTelemetryData")));
            const auto after = Clock::now();
            const bool ok = value.get() && CFGetTypeID(value.get()) == CFDictionaryGetTypeID();
            const auto dict = ok ? static_cast<CFDictionaryRef>(value.get()) : nullptr;
            OwnedCF<CFTypeRef> external(registry.Copy(CFSTR("ExternalConnected")));
            OwnedCF<CFTypeRef> charging(registry.Copy(CFSTR("IsCharging")));
            if (!ok) registry.Reset(); // Retry matching on the next scheduled line, never spin.
            std::wostringstream out;
            out << std::fixed << std::setprecision(3) << L"t_s=" << std::chrono::duration<double>(after - start).count();
            for (std::size_t k = 0; k < instant.size(); ++k) {
                const auto n = ParseTelemetryInteger(Get(dict, instant[k]));
                if (n.valid && previous[k].valid) { ++valid_pairs[k]; if (n.value != previous[k].value) ++changes[k]; }
                previous[k] = n;
                out << L" " << FromCF(instant[k]) << L"="; PrintInteger(out, n);
            }
            for (std::size_t k = 0; k < counts.size(); ++k) {
                const auto a = ParseTelemetryInteger(Get(dict, accumulated[k]));
                const auto c = ParseTelemetryInteger(Get(dict, counts[k]));
                out << L" " << FromCF(accumulated[k]) << L"="; PrintInteger(out, a);
                out << L" " << FromCF(counts[k]) << L"="; PrintInteger(out, c);
                out << L" " << FromCF(instant[k]) << L"_avg_mW=";
                if (a.valid && c.valid && old_accumulated[k].valid && old_counts[k].valid && c.value > old_counts[k].value) {
                    const long double da = static_cast<long double>(a.value) - old_accumulated[k].value;
                    const long double dc = static_cast<long double>(c.value) - old_counts[k].value;
                    out << da / dc;
                } else out << L"--";
                old_accumulated[k] = a; old_counts[k] = c;
            }
            out << L" ExternalConnected="; PrintInteger(out, ParseTelemetryInteger(external.get()));
            out << L" IsCharging="; PrintInteger(out, ParseTelemetryInteger(charging.get()));
            out << L" read_ms=";
            if (matched) out << std::chrono::duration<double, std::milli>(after - before).count(); else out << L"--";
            out << L" dictionary_ok=" << ok << L"\n"; output(out.str());
        }
    }
    std::wostringstream out; out << L"telemetry 相邻有效行变化次数（缺失不算变化，也不跨缺失行比较）：\n";
    for (std::size_t k = 0; k < instant.size(); ++k) out << FromCF(instant[k]) << L" changes=" << changes[k]
        << L" valid_pairs=" << valid_pairs[k] << L"\n";
    output(out.str());
}

const wchar_t* LastBatteryPowerSource() { return last_battery_source; }
const wchar_t* LastSystemPowerSource() { return last_system_source; }

void RunMemoryDiagnostics(void (*output)(const std::wstring&)) {
    if (!output) return;
    const auto start = Clock::now();
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_until(start + std::chrono::seconds(2 * i));
        std::uint64_t total = 0; std::size_t size = sizeof(total);
        const bool total_ok = sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0 && total;
        struct xsw_usage swap{}; size = sizeof(swap);
        const bool swap_ok = sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0;
        const host_t host = mach_host_self();
        vm_size_t page = 0;
        const auto page_status = host_page_size(host, &page);
        vm_statistics64_data_t vm{}; mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        const auto status = host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
        mach_port_deallocate(mach_task_self(), host);
        std::wostringstream out; out << std::fixed << std::setprecision(3)
            << L"--- mem t_s=" << std::chrono::duration<double>(Clock::now() - start).count() << L" ---\n";
        out << L"hw.memsize="; if (total_ok) out << total; else out << L"--";
        out << L" host_page_size="; if (page_status == KERN_SUCCESS) out << page; else out << L"--";
        out << L" vm_kernel_page_size=" << vm_kernel_page_size << L"\n";
        const auto bytes = [&](const wchar_t* name, std::uint64_t n) { out << name << L"=" << n << L" bytes GiB=" << static_cast<double>(n) / kGiB << L" "; };
        out << L"vm.swapusage: ";
        if (swap_ok) { bytes(L"total", swap.xsu_total); bytes(L"avail", swap.xsu_avail); bytes(L"used", swap.xsu_used);
            out << L"pagesize=" << swap.xsu_pagesize << L" encrypted=" << swap.xsu_encrypted; }
        else out << L"--";
        out << L"\nHOST_VM_INFO64 status=" << status << L" count=" << count << L"\n";
        if (status == KERN_SUCCESS) {
            const auto pages = [&](const wchar_t* name, std::uint64_t n) {
                out << name << L" pages=" << n << L" GiB=";
                if (page_status == KERN_SUCCESS && page) out << static_cast<long double>(n) * page / kGiB;
                else out << L"--";
                out << L"\n";
            };
            pages(L"free", vm.free_count); pages(L"active", vm.active_count); pages(L"inactive", vm.inactive_count);
            pages(L"speculative", vm.speculative_count); pages(L"throttled", vm.throttled_count); pages(L"wire", vm.wire_count);
            pages(L"purgeable", vm.purgeable_count); pages(L"external", vm.external_page_count); pages(L"internal", vm.internal_page_count);
            pages(L"compressor_page_count", vm.compressor_page_count);
            pages(L"total_uncompressed_pages_in_compressor", vm.total_uncompressed_pages_in_compressor);
            pages(L"zero_fill", vm.zero_fill_count); pages(L"reactivations", vm.reactivations);
            pages(L"pageins", vm.pageins); pages(L"pageouts", vm.pageouts); pages(L"faults", vm.faults);
            pages(L"cow_faults", vm.cow_faults); pages(L"lookups", vm.lookups); pages(L"hits", vm.hits); pages(L"purges", vm.purges);
            pages(L"decompressions", vm.decompressions); pages(L"compressions", vm.compressions);
            pages(L"swapins", vm.swapins); pages(L"swapouts", vm.swapouts);
            out << L"累计事件字段的 GiB 仅为计数乘页大小，不是当前驻留内存。\n";
            const auto candidate = [&](const wchar_t* name, long double n) {
                out << name << L" GiB="; if (std::isfinite(n)) out << n / kGiB; else out << L"--";
                out << L" percent="; if (std::isfinite(n) && total_ok) out << 100.0L * n / total; else out << L"--";
                out << L"\n";
            };
            if (page_status == KERN_SUCCESS && page) {
                // Use signed floating arithmetic before subtraction; no unsigned wrapping.
                const long double app = static_cast<long double>(vm.internal_page_count) - vm.purgeable_count;
                const long double wire = vm.wire_count, compressor = vm.compressor_page_count;
                candidate(L"A internal-purgeable+wire+compressor", (app + wire + compressor) * page);
                const long double b = static_cast<long double>(vm.active_count) + vm.inactive_count + vm.speculative_count
                    + wire + compressor - vm.purgeable_count - vm.external_page_count;
                candidate(L"B active+inactive+speculative+wire+compressor-purgeable-external", b * page);
                const long double excluded = static_cast<long double>(vm.free_count) + vm.speculative_count + vm.external_page_count;
                candidate(L"C memsize-(free+speculative+external)", total_ok ? static_cast<long double>(total) - excluded * page : NAN);
                candidate(L"D App(internal-purgeable)", app * page); candidate(L"D Wired", wire * page); candidate(L"D Compressed", compressor * page);
                candidate(L"E Cached(external+purgeable)", (static_cast<long double>(vm.external_page_count) + vm.purgeable_count) * page);
                const auto free = vm.free_count >= vm.speculative_count ? vm.free_count - vm.speculative_count : 0;
                candidate(L"F memsize-(free-speculative)-external", total_ok ?
                    std::clamp(static_cast<long double>(total) - (static_cast<long double>(free) + vm.external_page_count) * page,
                        0.0L, static_cast<long double>(total)) : NAN);
            }
        }
        output(out.str());
    }
}
std::wstring BuildProcessDiagnosticsReport() {
    @autoreleasepool {
    ProcessCollector collector;
    collector.Read(true); std::this_thread::sleep_for(std::chrono::seconds(1)); collector.Read(true);
    std::wostringstream out;
    out << L"timebase numer=" << MachTime().info.numer << L" denom=" << MachTime().info.denom
        << L" logical_cpus=" << Cores() << L"\nri_*_time 单位校准: 实测每单位秒数=" << ProcessCpuUnit().measured
        << L" Mach 每单位秒数=" << MachTime().Seconds(1) << L" 选用=" << (ProcessCpuUnit().mach ? L"Mach 时间" : L"纳秒")
        << L"\n进程 CPU 占全部逻辑核心\n"
        << L"listed=" << collector.listed << L" unreadable=" << collector.unreadable
        << L" denied=" << collector.denied << L" gone_or_other=" << collector.gone_or_other << L"\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(10, collector.ranked.size()); ++i) {
        const auto& row = collector.ranked[i];
        out << i + 1 << L" pid=" << row.pid << L" " << ProcessCollector::Name(row.pid) << L" CPU=" << row.percent << L"%\n";
    }
    return out.str();
    }
}
std::wstring BuildNetworkDiagnosticsReport(const Settings& settings) {
    @autoreleasepool {
    std::wostringstream out;
    out << BuildProbeDiagnosticsReport(settings); // System proxy settings lead the --net report.
    std::vector<Nic> rows; int error = 0;
    std::vector<unsigned char> buffer;
    if (!ReadNics(rows, error, buffer)) out << L"NET_RT_IFLIST2 查询失败 errno=" << error << L"\n";
    for (const Nic& row : rows) out << FromUtf8(row.name.c_str()) << L" index=" << row.index
        << L" type=" << row.type << L" flags=0x" << std::hex << row.flags << std::dec
        << L" rx=" << row.rx << L" tx=" << row.tx << L" include=" << row.include << L" " << row.reason << L"\n";
    return out.str();
    }
}
std::wstring BuildGpuDiagnosticsReport() {
    @autoreleasepool { std::wostringstream out; ReadGpu(&out); return out.str(); }
}
} } // namespace pulse::mac

namespace pulse { namespace mac { namespace {
enum class EnergyKind { None, Cpu, ETotal, PTotal, ECluster, PCluster, Gpu, Ane, Dram };
struct EnergyAlias { const wchar_t* name; EnergyKind kind; };
// Exact aliases have priority in this order. No substring guesses for GPU/ANE/DRAM.
constexpr EnergyAlias kEnergyAliases[] = {
    {L"CPU Energy", EnergyKind::Cpu}, {L"CPU Total Energy", EnergyKind::Cpu}, {L"CPU", EnergyKind::Cpu},
    {L"ECPU Energy", EnergyKind::ETotal}, {L"ECPU", EnergyKind::ETotal},
    {L"PCPU Energy", EnergyKind::PTotal}, {L"PCPU", EnergyKind::PTotal},
    {L"GPU Energy", EnergyKind::Gpu}, {L"GPU", EnergyKind::Gpu},
    {L"ANE Energy", EnergyKind::Ane}, {L"ANE", EnergyKind::Ane}, {L"Neural Engine Energy", EnergyKind::Ane},
    {L"DRAM Energy", EnergyKind::Dram}, {L"DRAM", EnergyKind::Dram}, {L"Memory Energy", EnergyKind::Dram}
};
bool ClusterName(const std::wstring& name, const wchar_t* prefix) {
    const std::wstring start(prefix);
    if (name.compare(0, start.size(), start) != 0) return false;
    std::size_t i = start.size(), first = i;
    while (i < name.size() && name[i] >= L'0' && name[i] <= L'9') ++i;
    return i > first && (i == name.size() || name.substr(i) == L" Energy");
}
EnergyKind Kind(const std::wstring& name) {
    for (const auto& alias : kEnergyAliases) if (name == alias.name) return alias.kind;
    if (ClusterName(name, L"ECPU")) return EnergyKind::ECluster;
    if (ClusterName(name, L"PCPU")) return EnergyKind::PCluster;
    return EnergyKind::None;
}
const wchar_t* KindName(EnergyKind kind) {
    switch (kind) {
    case EnergyKind::Cpu: return L"CPU total";
    case EnergyKind::ETotal: return L"ECPU total";
    case EnergyKind::PTotal: return L"PCPU total";
    case EnergyKind::ECluster: return L"ECPU cluster";
    case EnergyKind::PCluster: return L"PCPU cluster";
    case EnergyKind::Gpu: return L"GPU";
    case EnergyKind::Ane: return L"ANE";
    case EnergyKind::Dram: return L"DRAM";
    default: return L"未匹配";
    }
}
double EnergyScale(const std::wstring& unit) {
    if (unit == L"mJ") return 1e-3;
    if (unit == L"uJ" || unit == L"µJ" || unit == L"μJ") return 1e-6;
    if (unit == L"nJ") return 1e-9;
    return NAN;
}
class EnergyCollector {
    // Only ABI declarations are referenced from macmon sources.rs and the author's
    // IOReport article; implementation here is independent. No static IOReport link.
    using CopyChannelsFn = CFDictionaryRef (*)(CFStringRef, CFStringRef, std::uint64_t, std::uint64_t, std::uint64_t);
    using SubscribeFn = CFTypeRef (*)(void*, CFMutableDictionaryRef, CFMutableDictionaryRef*, std::uint64_t, CFTypeRef);
    using SamplesFn = CFDictionaryRef (*)(CFTypeRef, CFMutableDictionaryRef, CFTypeRef);
    using DeltaFn = CFDictionaryRef (*)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
    using StringFn = CFStringRef (*)(CFDictionaryRef);
    using IntegerFn = std::int64_t (*)(CFDictionaryRef, int);
    using IdFn = std::uint64_t (*)(CFDictionaryRef);
    void* library_ = nullptr;
    bool attempted_ = false;
    std::wstring error_;
    CopyChannelsFn copy_ = nullptr;
    SubscribeFn subscribe_ = nullptr;
    SamplesFn sample_ = nullptr;
    DeltaFn delta_ = nullptr;
    StringFn group_ = nullptr, subgroup_ = nullptr, name_ = nullptr, unit_ = nullptr;
    IntegerFn integer_ = nullptr;
    IdFn channel_id_ = nullptr, driver_id_ = nullptr;
    OwnedCF<CFMutableDictionaryRef> channels_, filtered_, active_request_, selected_;
    bool subscription_fallback_ = false;
    OwnedCF<CFTypeRef> subscription_;
    OwnedCF<CFDictionaryRef> previous_;
    std::uint64_t previous_at_ = 0, retry_at_ = 0;
    MeterStatus init_status_ = MeterStatus::CounterUnavailable;
    struct Row {
        std::wstring group, subgroup, name, unit;
        std::uint64_t driver = 0, channel = 0;
        CFDictionaryRef dictionary = nullptr; // Borrowed from the dictionary passed to Rows.
        EnergyKind kind = EnergyKind::None;
        double raw = NAN, watts = NAN;
    };
    template<class T> bool Symbol(T& fn, const char* name) {
        fn = reinterpret_cast<T>(dlsym(library_, name));
        if (!fn) error_ += L" 缺少 " + FromUtf8(name);
        return fn != nullptr;
    }
    bool Load() {
        if (attempted_) return copy_ && subscribe_ && sample_ && delta_ && group_ && subgroup_ && name_ && unit_ && integer_;
        attempted_ = true;
        library_ = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY | RTLD_LOCAL);
        if (!library_) { error_ = FromUtf8(dlerror()); return false; }
        bool ok = true;
        ok = Symbol(copy_, "IOReportCopyChannelsInGroup") && ok;
        ok = Symbol(subscribe_, "IOReportCreateSubscription") && ok;
        ok = Symbol(sample_, "IOReportCreateSamples") && ok;
        ok = Symbol(delta_, "IOReportCreateSamplesDelta") && ok;
        ok = Symbol(group_, "IOReportChannelGetGroup") && ok;
        ok = Symbol(subgroup_, "IOReportChannelGetSubGroup") && ok;
        ok = Symbol(name_, "IOReportChannelGetChannelName") && ok;
        ok = Symbol(unit_, "IOReportChannelGetUnitLabel") && ok;
        ok = Symbol(integer_, "IOReportSimpleGetIntegerValue") && ok;
        // IDs only dedupe totals; their absence must not disable every energy domain.
        channel_id_ = reinterpret_cast<IdFn>(dlsym(library_, "IOReportChannelGetChannelID"));
        driver_id_ = reinterpret_cast<IdFn>(dlsym(library_, "IOReportChannelGetDriverID"));
        if (!channel_id_ || !driver_id_) error_ += L" 缺少通道 ID 函数（按行号区分通道）";
        return ok;
    }
    bool Subscribe(CFMutableDictionaryRef channels) {
        subscription_.reset(); selected_.reset();
        CFMutableDictionaryRef selected = nullptr;
        subscription_.reset(subscribe_(nullptr, channels, &selected, 0, nullptr));
        selected_.reset(selected);
        if (subscription_.get() && selected_.get()) return true;
        subscription_.reset(); selected_.reset(); return false;
    }
    bool Ensure() {
        if (subscription_.get()) return true;
        if (!Load()) { init_status_ = MeterStatus::CounterUnavailable; return false; }
        if (NowMs() < retry_at_) return false;
        retry_at_ = NowMs() + 30000;
        subscription_fallback_ = false;
        OwnedCF<CFDictionaryRef> copied(copy_(CFSTR("Energy Model"), nullptr, 0, 0, 0));
        if (!copied.get()) { init_status_ = MeterStatus::NoMatch; return false; }
        channels_.reset(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, copied.get()));
        filtered_.reset(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, copied.get()));
        if (!channels_.get() || !filtered_.get()) { init_status_ = MeterStatus::ReadFailed; return false; }
        std::vector<Row> rows;
        if (!Rows(channels_.get(), NAN, false, rows)) { init_status_ = MeterStatus::NoMatch; return false; }
        const auto chosen = ChosenRows(rows);
        OwnedCF<CFMutableArrayRef> array(CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
        if (!array.get()) { init_status_ = MeterStatus::ReadFailed; return false; }
        for (const Row* row : chosen) CFArrayAppendValue(array.get(), row->dictionary);
        CFDictionarySetValue(filtered_.get(), CFSTR("IOReportChannels"), array.get());
        if (chosen.empty()) { init_status_ = MeterStatus::NoMatch; return false; }
        // Subscribe may modify its input, so keep the requested lists intact for diagnostics.
        active_request_.reset(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, filtered_.get()));
        if (active_request_.get() && Subscribe(active_request_.get())) return true;
        subscription_fallback_ = true;
        active_request_.reset(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, channels_.get()));
        if (active_request_.get() && Subscribe(active_request_.get())) return true;
        init_status_ = MeterStatus::ReadFailed; return false;
    }
    bool Rows(CFDictionaryRef sample, double seconds, bool delta, std::vector<Row>& rows) {
        const auto array = Get(sample, CFSTR("IOReportChannels"));
        if (!array || CFGetTypeID(array) != CFArrayGetTypeID()) return false;
        const auto list = static_cast<CFArrayRef>(array);
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
            const auto item = CFArrayGetValueAtIndex(list, i);
            if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID()) continue;
            const auto dict = static_cast<CFDictionaryRef>(item);
            Row row; row.dictionary = dict;
            row.group = FromCF(group_(dict)); row.subgroup = FromCF(subgroup_(dict));
            row.name = FromCF(name_(dict)); row.unit = FromCF(unit_(dict));
            if (driver_id_) row.driver = driver_id_(dict);
            row.channel = channel_id_ ? channel_id_(dict) : static_cast<std::uint64_t>(i) + 1;
            if (row.group == L"Energy Model") row.kind = Kind(row.name);
            if (delta && seconds > 0) {
                row.raw = static_cast<double>(integer_(dict, 0));
                const double scale = EnergyScale(row.unit);
                if (row.raw >= 0 && std::isfinite(scale)) row.watts = row.raw * scale / seconds;
            }
            rows.push_back(std::move(row));
        }
        return true;
    }
    void PrintChannels(std::wostream& out, CFDictionaryRef dictionary, const wchar_t* label) {
        std::vector<Row> rows;
        Rows(dictionary, NAN, false, rows);
        out << label << L" count=" << rows.size() << L"\n";
        for (const auto& row : rows) out << label << L" group=" << row.group << L" subgroup=" << row.subgroup
            << L" name=" << row.name << L" unit=" << row.unit << L" driver=" << row.driver
            << L" channel=" << row.channel << L" class=" << KindName(row.kind) << L"\n";
    }
    struct Domain { double watts = NAN; MeterStatus status = MeterStatus::NoMatch; };
    static Domain Combine(const std::vector<const Row*>& selected, bool warmup, bool sum) {
        if (selected.empty()) return {};
        Domain result; result.status = warmup ? MeterStatus::Warmup : MeterStatus::Valid;
        double watts = 0;
        std::map<std::pair<std::uint64_t, std::uint64_t>, const Row*> seen;
        for (const Row* row : selected) {
            if (!seen.emplace(std::make_pair(row->driver, row->channel), row).second) continue;
            if (!warmup && !std::isfinite(row->watts)) result.status = MeterStatus::ReadFailed;
            watts += row->watts;
        }
        if (!sum && seen.size() != 1) result.status = MeterStatus::ReadFailed; // Ambiguous total, don't double count.
        if (result.status == MeterStatus::Valid) result.watts = watts;
        return result;
    }
    static std::vector<const Row*> Choose(const std::vector<Row>& rows, EnergyKind kind,
                                          bool sum = false, std::uint64_t preferred_driver = 0) {
        std::vector<const Row*> selected;
        if (sum) {
            for (const auto& row : rows) if (row.kind == kind && std::isfinite(EnergyScale(row.unit))) selected.push_back(&row);
        } else {
            // GPU first searches aliases on the CPU-total driver, then the original order.
            for (int pass = 0; pass < (preferred_driver ? 2 : 1); ++pass) {
                for (const auto& alias : kEnergyAliases) if (alias.kind == kind) {
                    for (const auto& row : rows) if (row.kind == kind && row.name == alias.name &&
                        std::isfinite(EnergyScale(row.unit)) &&
                        (!preferred_driver || pass == 1 || row.driver == preferred_driver)) selected.push_back(&row);
                    if (!selected.empty()) return selected;
                }
            }
        }
        return selected;
    }
    static std::uint64_t CpuDriver(const std::vector<Row>& rows) {
        const auto cpu = Choose(rows, EnergyKind::Cpu);
        if (cpu.empty() || !cpu.front()->driver) return 0;
        const Row* first = cpu.front();
        // Ambiguous CPU totals must not impose an arbitrary driver preference.
        for (const Row* row : cpu) if (row->driver != first->driver || row->channel != first->channel) return 0;
        return first->driver;
    }
    static std::vector<const Row*> ChosenRows(const std::vector<Row>& rows) {
        auto chosen = Choose(rows, EnergyKind::Cpu);
        const auto append = [&](const std::vector<const Row*>& items) {
            for (const Row* row : items) if (std::find(chosen.begin(), chosen.end(), row) == chosen.end()) chosen.push_back(row);
        };
        if (chosen.empty()) {
            auto e = Choose(rows, EnergyKind::ETotal), p = Choose(rows, EnergyKind::PTotal);
            if (e.empty()) e = Choose(rows, EnergyKind::ECluster, true);
            if (p.empty()) p = Choose(rows, EnergyKind::PCluster, true);
            append(e); append(p);
        }
        append(Choose(rows, EnergyKind::Gpu, false, CpuDriver(rows)));
        append(Choose(rows, EnergyKind::Ane)); append(Choose(rows, EnergyKind::Dram));
        return chosen;
    }
    static Domain Select(const std::vector<Row>& rows, EnergyKind kind, bool warmup, bool sum = false,
                         std::uint64_t preferred_driver = 0) {
        return Combine(Choose(rows, kind, sum, preferred_driver), warmup, sum);
    }
    static Domain Sum(const std::vector<Domain>& domains) {
        Domain result; double watts = 0;
        for (const auto& domain : domains) {
            if (domain.status != MeterStatus::Valid) { result.status = domain.status; return result; }
            watts += domain.watts;
        }
        result.status = MeterStatus::Valid; result.watts = watts; return result;
    }
    struct Values { Domain cpu, gpu, ane, dram, package; };
    static Values Evaluate(const std::vector<Row>& rows, bool warmup) {
        Domain cpu = Select(rows, EnergyKind::Cpu, warmup);
        if (cpu.status == MeterStatus::NoMatch) {
            auto e = Select(rows, EnergyKind::ETotal, warmup);
            auto p = Select(rows, EnergyKind::PTotal, warmup);
            if (e.status == MeterStatus::NoMatch) e = Select(rows, EnergyKind::ECluster, warmup, true);
            if (p.status == MeterStatus::NoMatch) p = Select(rows, EnergyKind::PCluster, warmup, true);
            // Apple Silicon has both cluster families. Missing one isn't a valid CPU total.
            cpu = Sum({e, p});
        }
        const auto gpu = Select(rows, EnergyKind::Gpu, warmup, false, CpuDriver(rows));
        const auto ane = Select(rows, EnergyKind::Ane, warmup);
        const auto dram = Select(rows, EnergyKind::Dram, warmup);
        const auto package = Sum({cpu, gpu, ane}); // Missing ANE remains unknown, never an assumed zero.
        return {cpu, gpu, ane, dram, package};
    }
    static void PrintValues(std::wostream& out, const wchar_t* label, const Values& v, double seconds) {
        out << label << L" interval_s=" << seconds;
        const auto field = [&](const wchar_t* name, const Domain& d) {
            out << L" " << name << L"=";
            if (std::isfinite(d.watts)) out << d.watts; else out << L"--";
            out << L" status=" << static_cast<int>(d.status);
        };
        field(L"CPU_W", v.cpu); field(L"GPU_W", v.gpu); field(L"ANE_W", v.ane);
        field(L"DRAM_W", v.dram); field(L"pkg_W", v.package); out << L"\n";
    }
public:
    void Benchmark(std::wostream& out) {
        if (!Ensure()) { out << L"IOReport benchmark unavailable status=" << static_cast<int>(init_status_) << L"\n"; return; }
        OwnedCF<CFMutableDictionaryRef> full_request(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, channels_.get()));
        OwnedCF<CFMutableDictionaryRef> filter_request(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, filtered_.get()));
        if (!full_request.get() || !filter_request.get()) { out << L"IOReport benchmark allocation failed\n"; return; }
        CFMutableDictionaryRef full_actual = nullptr, filter_actual = nullptr;
        OwnedCF<CFMutableDictionaryRef> full_channels, filter_channels;
        OwnedCF<CFTypeRef> full_sub(subscribe_(nullptr, full_request.get(), &full_actual, 0, nullptr));
        full_channels.reset(full_actual);
        OwnedCF<CFTypeRef> filter_sub(subscribe_(nullptr, filter_request.get(), &filter_actual, 0, nullptr));
        filter_channels.reset(filter_actual);
        // The filtered arm deliberately has no full-group fallback, or the comparison would lie.
        if (!full_sub.get() || !full_channels.get() || !filter_sub.get() || !filter_channels.get()) {
            out << L"IOReport benchmark unavailable full_ready=" << (full_sub.get() && full_channels.get())
                << L" filtered_ready=" << (filter_sub.get() && filter_channels.get()) << L"（不以全组代替过滤臂）\n"; return;
        }
        const auto measure = [&](CFTypeRef sub, CFMutableDictionaryRef channels, const wchar_t* label) {
            std::array<OwnedCF<CFDictionaryRef>, 6> samples;
            std::array<std::uint64_t, 6> stamps{};
            std::vector<double> sampling, parsing;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const auto before = mach_absolute_time();
                samples[i].reset(sample_(sub, channels, nullptr));
                const auto after = mach_absolute_time();
                stamps[i] = before + (after - before) / 2;
                if (!samples[i].get()) { out << label << L" sample_ms=-- parse_ms=-- CreateSamples failed\n"; return; }
                if (i) sampling.push_back(MachTime().Seconds(after - before) * 1000.0);
            }
            Values values;
            for (std::size_t i = 1; i < samples.size(); ++i) {
                const double seconds = MachTime().Seconds(stamps[i] - stamps[i - 1]);
                const auto before = Clock::now();
                {
                    OwnedCF<CFDictionaryRef> difference(delta_(samples[i - 1].get(), samples[i].get(), nullptr));
                    std::vector<Row> rows;
                    if (!difference.get() || !Rows(difference.get(), seconds, true, rows)) {
                        out << label << L" sample_ms=-- parse_ms=-- delta/parse failed\n"; return;
                    }
                    values = Evaluate(rows, false);
                }
                parsing.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
            }
            std::sort(sampling.begin(), sampling.end()); std::sort(parsing.begin(), parsing.end());
            out << label << L" sample_ms=" << sampling[2] << L" parse_ms=" << parsing[2]
                << L"（CreateSamples 连续6次丢第1次，差分+解析5次，各取中位）\n";
            PrintValues(out, L"benchmark-last-delta", values, MachTime().Seconds(stamps[5] - stamps[4]));
        };
        measure(full_sub.get(), full_channels.get(), L"benchmark-full");
        measure(filter_sub.get(), filter_channels.get(), L"benchmark-filtered");
        // Read both subscriptions over overlapping ~1 s windows, independently from the
        // back-to-back timing runs whose tiny energy deltas may legitimately be zero.
        OwnedCF<CFDictionaryRef> full_first, filter_first;
        const auto capture = [&](CFTypeRef sub, CFMutableDictionaryRef channels, OwnedCF<CFDictionaryRef>& dst) {
            const auto before = mach_absolute_time(); dst.reset(sample_(sub, channels, nullptr));
            const auto after = mach_absolute_time(); return before + (after - before) / 2;
        };
        const auto full_at = capture(full_sub.get(), full_channels.get(), full_first);
        const auto filter_at = capture(filter_sub.get(), filter_channels.get(), filter_first);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto compare = [&](CFTypeRef sub, CFMutableDictionaryRef channels, CFDictionaryRef first,
                                 std::uint64_t at, const wchar_t* label, bool dump) {
            OwnedCF<CFDictionaryRef> last;
            const auto now = capture(sub, channels, last);
            const double seconds = now > at ? MachTime().Seconds(now - at) : NAN;
            OwnedCF<CFDictionaryRef> difference(first && last.get() ? delta_(first, last.get(), nullptr) : nullptr);
            std::vector<Row> rows;
            if (!difference.get() || !Rows(difference.get(), seconds, true, rows)) { out << label << L" read failed\n"; return; }
            PrintValues(out, label, Evaluate(rows, false), seconds);
            if (dump) for (const Row& row : rows) out << L"full group=" << row.group << L" subgroup=" << row.subgroup
                << L" name=" << row.name << L" unit=" << row.unit << L" driver=" << row.driver << L" channel=" << row.channel
                << L" delta=" << row.raw << L" W=" << row.watts << L" class=" << KindName(row.kind) << L"\n";
        };
        compare(full_sub.get(), full_channels.get(), full_first.get(), full_at, L"compare-full", true);
        compare(filter_sub.get(), filter_channels.get(), filter_first.get(), filter_at, L"compare-filtered", false);
    }
    ~EnergyCollector() { Reset(); if (library_) dlclose(library_); }
    void Reset() {
        previous_.reset(); previous_at_ = 0;
        subscription_.reset(); selected_.reset(); active_request_.reset(); filtered_.reset(); channels_.reset(); retry_at_ = 0;
        subscription_fallback_ = false;
    }
    void Read(Snapshot& snapshot, std::wostream* out = nullptr, double* ane_out = nullptr) {
        if (ane_out) *ane_out = NAN;
        Timing timing("IOReport");
        auto fail = [&](MeterStatus status) { snapshot.pkg_meter = snapshot.cpu_meter = snapshot.gpu_meter = snapshot.mem_meter = status; };
        const bool ready = Ensure();
        if (out) {
            PrintChannels(*out, channels_.get(), L"requested");
            PrintChannels(*out, filtered_.get(), L"filtered-request");
            PrintChannels(*out, selected_.get(), L"subscribed");
            *out << L"subscription fallback_full=" << subscription_fallback_
                << (subscription_fallback_ ? L"（过滤订阅失败，尝试回退全组）" : L"（未使用全组回退）") << L"\n";
        }
        if (!ready) {
            fail(init_status_);
            if (out) *out << L"IOReport loaded=" << (library_ != nullptr) << L" ready=0 status="
                << static_cast<int>(init_status_) << L" fallback_full=" << subscription_fallback_ << L" " << error_ << L"\n";
            return;
        }
        const auto before = mach_absolute_time();
        OwnedCF<CFDictionaryRef> current;
        { Timing sample_timing("IOReport sample"); current.reset(sample_(subscription_.get(), selected_.get(), nullptr)); }
        const auto after = mach_absolute_time();
        const auto now = before + (after - before) / 2;
        if (!current.get()) {
            previous_.reset(); previous_at_ = 0; fail(MeterStatus::ReadFailed);
            if (out) *out << L"IOReportCreateSamples 失败\n";
            return;
        }
        const bool warmup = !previous_.get() || now <= previous_at_;
        const double seconds = !warmup ? MachTime().Seconds(now - previous_at_) : NAN;
        {
        Timing parse_timing("IOReport parse");
        OwnedCF<CFDictionaryRef> difference(!warmup ? delta_(previous_.get(), current.get(), nullptr) : nullptr);
        std::vector<Row> rows;
        const bool read_ok = Rows(warmup ? current.get() : difference.get(), seconds, !warmup, rows);
        // Keep a retained current sample as baseline. No sample-index correspondence is assumed.
        previous_.reset(static_cast<CFDictionaryRef>(CFRetain(current.get()))); previous_at_ = now;
        if (!read_ok) {
            fail(MeterStatus::ReadFailed);
            if (out) *out << L"IOReport 差分或 IOReportChannels 读取失败；实际原始结构="
                << Describe(warmup ? current.get() : difference.get()) << L"\n";
            return;
        }
        const auto values = Evaluate(rows, warmup);
        const auto cpu = values.cpu, gpu = values.gpu, ane = values.ane, dram = values.dram, package = values.package;
        if (ane_out) *ane_out = ane.watts;
        snapshot.cpu_w = cpu.watts; snapshot.cpu_meter = cpu.status;
        snapshot.gpu_w = gpu.watts; snapshot.gpu_meter = gpu.status;
        snapshot.mem_w = dram.watts; snapshot.mem_meter = dram.status;
        snapshot.pkg_w = package.watts; snapshot.pkg_meter = package.status;
        if (package.status == MeterStatus::Valid) {
            snapshot.pkg_power_from_energy = true;
            snapshot.pkg_energy_interval_s = seconds;
            // Shared diagnostic field is explicitly pWh, not raw IOReport units.
            snapshot.pkg_energy_delta_pwh = package.watts * seconds / 3.6e-9;
        }
        if (out) {
            *out << L"IOReport loaded=1 ready=1 interval_s=" << seconds << L" warmup=" << warmup << error_ << L"\n";
            *out << L"实际样本通道及本次差分：\n";
            for (const auto& row : rows) *out << L"group=" << row.group << L" subgroup=" << row.subgroup
                << L" name=" << row.name << L" unit=" << row.unit << L" driver=" << row.driver
                << L" channel=" << row.channel << L" delta=" << row.raw << L" W=" << row.watts
                << L" class=" << KindName(row.kind) << L"\n";
            *out << L"CPU=" << cpu.watts << L" GPU=" << gpu.watts << L" ANE=" << ane.watts
                << L" DRAM=" << dram.watts << L" pkg(CPU+GPU+ANE)=" << package.watts << L"\n"
                << L"候选表（优先总通道；簇通道仅总通道缺失时使用）：\n";
            for (const auto& alias : kEnergyAliases) *out << alias.name << L" -> " << KindName(alias.kind) << L"\n";
            *out << L"ECPU[0-9]+[ Energy] / PCPU[0-9]+[ Energy] -> clusters\n";
        }
        } // IOReport parse
    }
};
} // anonymous namespace
// Diagnostic-only bridge: reuse the existing BatteryRegistry and integer parser.
void RunSmcDiagnostics(int samples, void (*output)(const std::wstring&)) {
    struct Context { BatteryRegistry battery; EnergyCollector energy; } context;
    detail::RunSmcDiagnosticsWithBattery(samples, output, [](void* context) -> std::wstring {
        @autoreleasepool {
            auto& state = *static_cast<Context*>(context);
            auto& battery = state.battery;
            battery.Ensure();
            OwnedCF<CFTypeRef> current(battery.Copy(CFSTR("InstantAmperage")));
            OwnedCF<CFTypeRef> voltage(battery.Copy(CFSTR("Voltage")));
            OwnedCF<CFTypeRef> external(battery.Copy(CFSTR("ExternalConnected")));
            std::wostringstream out;
            out << L" InstantAmperage="; PrintInteger(out, ParseTelemetryInteger(current.get()));
            out << L" Voltage="; PrintInteger(out, ParseTelemetryInteger(voltage.get()));
            out << L" ExternalConnected="; PrintInteger(out, ParseTelemetryInteger(external.get()));
            if (!voltage.get()) battery.Reset();
            Snapshot sample; double ane = NAN;
            state.energy.Read(sample, nullptr, &ane);
            out << L" IOReport_CPU_W=" << sample.cpu_w << L" IOReport_GPU_W=" << sample.gpu_w
                << L" IOReport_ANE_W=" << ane << L" IOReport_DRAM_W=" << sample.mem_w;
            return out.str();
        }
    }, &context);
}
} } // namespace pulse::mac

namespace pulse {
struct Metrics::Impl {
    std::mutex mutex;
    Wants wants;
    bool paused = false, suspended = false, had_mode = false, full_mode = false;
    mac::BatteryRegistry battery;
    mac::BatteryIopsCache battery_iops;
    mac::BatterySmc battery_smc;
    mac::BatteryStatistics battery_stats;
    mac::CpuCollector cpu;
    mac::ProcessCollector processes;
    mac::NetworkCollector network;
    mac::DiskCollector disk;
    mac::EnergyCollector energy;
    AcDischargeDebounce debounce;
    NetProbe probe;
    double self_previous = NAN;
    std::uint64_t self_at = 0;
    void Reset() {
        cpu.previous.clear(); processes.previous.clear(); processes.ranked.clear();
        network.Reset(); disk.previous.clear(); disk.at = 0;
        battery_stats.BreakInterval();
        energy.Reset(); battery_iops.Reset(); debounce.Reset(); self_previous = NAN; self_at = 0;
    }
    Snapshot Take(bool full) {
        std::lock_guard<std::mutex> lock(mutex);
        mac::Timing total("Sample total");
        @autoreleasepool {
            // Panel toggles preserve CPU, network and battery intervals. IOReport
            // is reset below and needs one warmup sample after each mode switch.
            if (!had_mode || full_mode != full) {
                processes.previous.clear(); processes.ranked.clear(); energy.Reset();
                full_mode = full; had_mode = true;
            }
            Snapshot s;
            s.cpu_percent = s.mem_percent = NAN;
            s.down_bps = s.up_bps = NAN;
            s.rx_total = network.total_rx; s.tx_total = network.total_tx;
            if (!paused && !suspended) {
                mac::ReadBattery(s, wants, debounce, battery, battery_iops, battery_smc, battery_stats);
                if (wants.chip_power) energy.Read(s);
                cpu.Read(s); mac::ReadMemory(s, wants.memory_detail);
                if (full && wants.processes) {
                    processes.Read(true); s.process_count = processes.listed;
                    for (std::size_t i = 0; i < std::min<std::size_t>(3, processes.ranked.size()); ++i) {
                        const auto& row = processes.ranked[i];
                        s.top_procs.push_back({mac::ProcessCollector::Name(row.pid), row.percent});
                    }
                } else {
                    processes.previous.clear();
                    if (wants.process_count) { processes.Read(false); s.process_count = processes.listed; }
                }
                if (wants.gpu_usage) s.gpu_percent = mac::ReadGpu();
                network.Read(s);
                if (wants.disk_io) disk.Read(s);
                if (wants.sys_disk) mac::ReadSystemDisk(s);
                if (wants.uptime) mac::ReadUptime(s);
                const double self_now = mac::SelfCpuSeconds();
                const auto now = mach_absolute_time();
                if (self_at && now > self_at && self_now >= self_previous)
                    s.self_cpu_percent = 100.0 * (self_now - self_previous) / mac::MachTime().Seconds(now - self_at) / mac::Cores();
                self_previous = self_now; self_at = now;
            }
            const auto view = probe.Get();
            s.net_state = view.state; s.latency_ms = view.latency_ms;
            s.offline_since_unix = view.offline_since_unix; s.offline_by_latency = view.offline_by_latency;
            s.offline_reason = view.offline_reason; s.offline_latency_ms = view.offline_latency_ms;
            return s;
        }
    }
};
Metrics::Metrics() : impl_(new Impl) {}
Metrics::~Metrics() { impl_->probe.Stop(); delete impl_; }
void Metrics::Start(const Settings& settings, EventCallback cb, void* user) { impl_->probe.Start(settings, cb, user); }
Snapshot Metrics::Sample(bool full) { return impl_->Take(full); }
void Metrics::SetWants(const Wants& wants) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!wants.chip_power) impl_->energy.Reset();
    if (!wants.processes) { impl_->processes.previous.clear(); impl_->processes.ranked.clear(); }
    if (!wants.disk_io) { impl_->disk.previous.clear(); impl_->disk.at = 0; }
    impl_->wants = wants;
}
void Metrics::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->battery_iops.Reset();
    if (impl_->paused != paused) { impl_->paused = paused; impl_->Reset(); }
}
std::wstring Metrics::LogPath() const { return impl_->probe.LogPath(); }
void Metrics::SetSlowThreshold(int slow_ms) { impl_->probe.SetSlowThreshold(slow_ms); }
void Metrics::SetProbeEnabled(bool enabled) { impl_->probe.SetEnabled(enabled); }
void Metrics::OnSuspend() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->suspended = true; impl_->Reset(); }
    impl_->probe.OnSuspend();
}
void Metrics::OnResume() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->suspended = false; impl_->Reset(); impl_->battery_smc.Close(); }
    impl_->probe.OnResume();
}
std::wstring BuildDgpuGateDiagnosticsReport() { return L"不适用：macOS 阶段 0 不采集 NVIDIA 独显门控。\n"; }
std::wstring BuildDgpuDiagnosticsReport(bool allow_wake, void (*before_nvml)(const std::wstring&)) {
    (void)allow_wake; (void)before_nvml;
    return L"不适用：macOS 阶段 0 不加载 NVML，不唤醒独显。\n";
}
std::wstring BuildPowerDiagnosticsReport() {
    @autoreleasepool {
        std::wostringstream out;
        out << L"Pulse macOS 阶段 1 功耗诊断\n"
            << L"lowPowerModeEnabled=" << NSProcessInfo.processInfo.lowPowerModeEnabled << L"\n";
        mac::OwnedCF<CFMutableDictionaryRef> props(mac::BatteryProperties());
        out << L"AppleSmartBattery 全部属性（含 PowerTelemetryData，不映射到 Snapshot）：\n";
        mac::DumpDictionary(out, props.get());
        mac::OwnedCF<CFTypeRef> info(IOPSCopyPowerSourcesInfo());
        mac::OwnedCF<CFArrayRef> sources(info.get() ? IOPSCopyPowerSourcesList(info.get()) : nullptr);
        out << L"IOPSCopyPowerSourcesInfo=" << mac::Describe(info.get()) << L"\n";
        if (sources.get()) for (CFIndex i = 0; i < CFArrayGetCount(sources.get()); ++i) {
            out << L"IOPS description #" << i << L"\n";
            mac::DumpDictionary(out, IOPSGetPowerSourceDescription(info.get(), CFArrayGetValueAtIndex(sources.get(), i)));
        }
        out << L"Battery key candidates: Voltage; InstantAmperage/Amperage; AppleRawCurrentCapacity; "
            << L"AppleRawMaxCapacity; DesignCapacity; CycleCount; BatteryInstalled; ExternalConnected; IsCharging\n"
            << L"PowerTelemetryData 的 SystemPowerIn/SystemLoad/BatteryPower/AdapterEfficiencyLoss 仅原样诊断，单位未假定。\n";
        out << L"\nSMC (read-only selector=2 commands=5/8/9; no enumeration):\n";
        mac::SmcReader smc;
        if (smc.Open(out)) for (auto key : {mac::Code("PSTR"), mac::Code("PPBR"), mac::Code("PDTR")}) {
            double value = NAN;
            out << mac::FourText(key) << L"_W=";
            if (smc.ReadFloat(key, value)) out << value; else out << L"--";
            out << L"\n";
        }
        mac::BatteryRegistry battery; mac::BatteryIopsCache iops;
        mac::BatterySmc battery_smc; mac::BatteryStatistics stats; AcDischargeDebounce debounce;
        Snapshot battery_sample;
        mac::ReadBattery(battery_sample, Wants{}, debounce, battery, iops, battery_smc, stats);
        out << L"power_w=" << battery_sample.power_w << L" battery_power_source=" << mac::LastBatteryPowerSource() << L"\n"
            << L"system_w=" << battery_sample.system_w << L" system_power_source=" << mac::LastSystemPowerSource() << L"\n";
        mac::EnergyCollector energy;
        Snapshot first, second;
        energy.Read(first, &out);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        energy.Read(second, &out);
        energy.Benchmark(out);
        return out.str();
    }
}
int WritePowerDiagnosticsFile(std::wstring* path_out, std::wstring* error_out) {
    if (path_out) path_out->clear();
    if (error_out) error_out->clear();
    @autoreleasepool {
        NSString* directory = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Pulse"];
        NSError* error = nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error]) {
            if (error_out) *error_out = mac::ToWide(error.localizedDescription);
            return 1;
        }
        NSString* name = [@"power-" stringByAppendingString:NSUUID.UUID.UUIDString];
        NSString* file = [directory stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"txt"]];
        const auto report = mac::Utf8(BuildPowerDiagnosticsReport());
        NSMutableData* data = [NSMutableData dataWithBytes:"\xef\xbb\xbf" length:3];
        [data appendBytes:report.data() length:report.size()];
        if (![data writeToFile:file options:NSDataWritingAtomic error:&error]) {
            if (error_out) *error_out = mac::ToWide(error.localizedDescription);
            return 1;
        }
        if (path_out) *path_out = mac::ToWide(file);
        return 0;
    }
}
} // namespace pulse
