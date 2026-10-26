#include "ui_model.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <tuple>
#include <utility>

namespace pulse::ui {
namespace {
// Both the floating window and the taskbar own a Model. All model updates are
// on the UI thread; share this lifetime flag across modes, components and models.
bool gpuTempSeen = false;
bool Known(double v) { return std::isfinite(v) && v >= 0; }
float Fraction(double v) {
    return Known(v) ? static_cast<float>(std::round(std::clamp(v, 0.0, 100.0)) / 100.0) : -1.0f;
}
std::vector<TimePart> Parts(double seconds) {
    if (!Known(seconds)) return {};
    const auto minutes = static_cast<long long>(std::round(std::min(seconds, 360000.0) / 60));
    if (minutes >= 6000) return {{L"99+", T(Key::Hours)}};
    if (minutes >= 60) return {{std::to_wstring(minutes / 60), T(Key::Hours)},
                             {std::to_wstring(minutes % 60), T(Key::MinuteShort)}};
    return {{std::to_wstring(minutes), T(Key::Minutes)}};
}
Speed FormatSpeed(double bps) {
    if (!Known(bps)) return {L"--", T(Key::Kbps)};
    const std::wstring* units[] = {&T(Key::Kbps), &T(Key::Mbps), &T(Key::Gbps)};
    double n = bps / 1024;
    int i = 0;
    while (n >= 1000 && i < 2) { n /= 1024; ++i; }
    return {Number(n, n >= 100 ? 0 : n >= 10 ? 1 : i ? 2 : 1), *units[i]};
}
std::wstring Bytes(uint64_t bytes) {
    const std::wstring* units[] = {&T(Key::Byte), &T(Key::Kb), &T(Key::Mb), &T(Key::Gb), &T(Key::Tb)};
    double n = static_cast<double>(bytes);
    int i = 0;
    while (n >= 1024 && i < 4) { n /= 1024; ++i; }
    return Number(n, i == 0 || n >= 100 ? 0 : 1) + L" " + *units[i];
}
std::wstring Percent(double v, int decimals) {
    return Known(v) ? Number(std::clamp(v, 0.0, 100.0), decimals) + L"%" : L"--";
}
std::wstring Clock(std::int64_t unixTime) {
    const auto now = static_cast<std::time_t>(unixTime);
    std::tm local{};
#if defined(_MSC_VER)
    localtime_s(&local, &now);
#else
    if (const auto* t = std::localtime(&now)) local = *t;
#endif
    wchar_t buffer[16]{};
    std::swprintf(buffer, 16, L"%02d:%02d", local.tm_hour, local.tm_min);
    return buffer;
}
float ChartValue(double v) {
    return Known(v) ? static_cast<float>(std::min(v, 1.0e15)) : -1.0f;
}
} // namespace

std::wstring Number(double value, int decimals) {
    if (!Known(value)) return L"--";
    wchar_t buffer[96]{};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.*f", decimals, value);
    return buffer;
}
std::wstring Duration(double seconds) {
    auto parts = Parts(seconds);
    if (parts.empty()) return L"--";
    std::wstring result;
    for (const auto& p : parts) {
        if (!result.empty()) result += L" ";
        result += p.value + L" " + p.unit;
    }
    return result;
}
std::wstring ShortDuration(double seconds) {
    if (!Known(seconds)) return L"--";
    const auto m = static_cast<int>(std::round(std::min(seconds, 360000.0) / 60));
    if (m >= 6000) return L"99+";
    wchar_t buffer[32]{};
    std::swprintf(buffer, 32, L"%d:%02d", m / 60, m % 60);
    return buffer;
}

std::wstring OutageDuration(std::uint64_t seconds) {
    if (seconds >= 3600) return std::to_wstring(seconds / 3600) + T(Key::HourJoin) +
        std::to_wstring(seconds / 60 % 60) + T(Key::MinuteSuffix);
    if (seconds >= 60) return std::to_wstring(seconds / 60) + T(Key::MinuteJoin) +
        std::to_wstring(seconds % 60) + T(Key::SecondSuffix);
    return std::to_wstring(seconds) + T(Key::SecondSuffix);
}
std::wstring OfflineReasonText(OfflineReason reason, double latencyMs) {
    switch (reason) {
    case OfflineReason::Timeout: return T(Key::Timeout);
    case OfflineReason::HighLatency: return i18n::Format(Key::HighLatency, {Number(latencyMs, 0)});
    case OfflineReason::LinkDisconnected: return T(Key::LinkDisconnected);
    case OfflineReason::NoResponse: return T(Key::NoResponse);
    default: return T(Key::UnknownReason);
    }
}
std::wstring NetworkEventText(const NetEvent& event) {
    if (event.recovered) return i18n::Format(Key::OutageEvent,
        {T(Key::NetworkRestored), Clock(event.event_unix), OutageDuration(event.outage_duration_s)});
    return i18n::Format(Key::DisconnectedEvent,
        {Clock(event.offline_start_unix), OfflineReasonText(event.reason, event.latency_ms)});
}

void Components::Reset() {
    for (size_t i = 0; i < full.size(); ++i) full[i] = FullComponents[i].enabled;
    for (size_t i = 0; i < compact.size(); ++i) compact[i] = CompactComponents[i].enabled;
    for (size_t i = 0; i < taskbar.size(); ++i) taskbar[i] = TaskbarComponents[i].enabled;
}
Wants CollectionWants(const Components& components, DisplayMode display, bool details, PowerTrace trace) {
    Components c = components;
    const bool compact = display != DisplayMode::Full && !details;
    if (display == DisplayMode::Taskbar) c.compact = c.taskbar;
    const bool package = NeedsPackagePower(trace);
    Wants w;
    w.chip_power = compact ? c.Enabled(CompactComponent::Power) && package :
        c.Enabled(FullComponent::Hardware) || (c.Enabled(FullComponent::PowerChart) && package);
    w.gpu_usage = compact ? c.Enabled(CompactComponent::Gpu) : c.Enabled(FullComponent::Hardware);
    w.dgpu_power = display == DisplayMode::Full && c.Enabled(FullComponent::Hardware);
    w.processes = !compact && c.Enabled(FullComponent::Processes);
    w.process_count = !compact && c.Enabled(FullComponent::SystemInfo);
    w.power_scheme = !compact && c.Enabled(FullComponent::Battery);
    w.memory_detail = !compact && c.Enabled(FullComponent::SystemInfo);
    w.disk_io = compact ? c.Enabled(CompactComponent::DiskIO) : c.Enabled(FullComponent::DiskIO);
    w.sys_disk = !compact && c.Enabled(FullComponent::SystemDisk);
    w.cpu_freq = compact ? c.Enabled(CompactComponent::CpuFrequency) : c.Enabled(FullComponent::SystemInfo);
    w.uptime = !compact && c.Enabled(FullComponent::SystemInfo);
    w.gpu_memory = !compact && c.Enabled(FullComponent::GpuMemory);
    w.gpu_temp = compact ? c.Enabled(CompactComponent::GpuTemp) : c.Enabled(FullComponent::GpuMemory);
    w.wifi = !compact && c.Enabled(FullComponent::Wifi);
    w.battery_health = !compact && c.Enabled(FullComponent::BatteryHealth);
    w.fps = compact ? c.Enabled(CompactComponent::Fps) : c.Enabled(FullComponent::Fps);
    if (display == DisplayMode::Taskbar && details) {
        const auto strip = CollectionWants(components, DisplayMode::Taskbar, false, trace);
        w.chip_power = w.chip_power || strip.chip_power;
        w.gpu_usage = w.gpu_usage || strip.gpu_usage;
        w.disk_io = w.disk_io || strip.disk_io;
        w.cpu_freq = w.cpu_freq || strip.cpu_freq;
        w.fps = w.fps || strip.fps;
        w.gpu_temp = w.gpu_temp || strip.gpu_temp;
    }
    return w;
}

bool View::Equals(const View& r, bool compact, const Components& c) const {
    if (compact) {
        if (c.Enabled(CompactComponent::Battery) &&
            std::tie(mode, supplemental, acOnline, battery, percent) !=
            std::tie(r.mode, r.supplemental, r.acOnline, r.battery, r.percent)) return false;
        if (c.Enabled(CompactComponent::Eta)) {
            if (compactEtaVisible != r.compactEtaVisible) return false;
            if (compactEtaVisible &&
                std::tie(mode, compactEtaLabel, compactEtaValue, compactEtaFromSystem) !=
                std::tie(r.mode, r.compactEtaLabel, r.compactEtaValue, r.compactEtaFromSystem)) return false;
        }
        if (c.Enabled(CompactComponent::Power) &&
            std::tie(mode, supplemental, low, powerTrace, powerLabel, powerValue) !=
            std::tie(r.mode, r.supplemental, r.low, r.powerTrace, r.powerLabel, r.powerValue)) return false;
        if (c.Enabled(CompactComponent::Cpu) && cpuValue != r.cpuValue) return false;
        if (c.Enabled(CompactComponent::Memory) && memoryValue != r.memoryValue) return false;
        if (c.Enabled(CompactComponent::Gpu) && gpuUsage != r.gpuUsage) return false;
        if (c.Enabled(CompactComponent::GpuTemp) &&
            std::tie(gpuTemp, gpuTempLevel) != std::tie(r.gpuTemp, r.gpuTempLevel)) return false;
        if (c.Enabled(CompactComponent::Network) &&
            std::tie(compactUp, compactDown) != std::tie(r.compactUp, r.compactDown)) return false;
        if (c.Enabled(CompactComponent::Latency) &&
            std::tie(netState, latency) != std::tie(r.netState, r.latency)) return false;
        if (c.Enabled(CompactComponent::DiskIO) &&
            std::tie(diskRead, diskWrite) != std::tie(r.diskRead, r.diskWrite)) return false;
        if (c.Enabled(CompactComponent::CpuFrequency) && cpuFrequency != r.cpuFrequency) return false;
        if (c.Enabled(CompactComponent::Fps) &&
            std::tie(fps, fpsAdmin) != std::tie(r.fps, r.fpsAdmin)) return false;
        return true;
    }
    if (clock != r.clock) return false; // Always-visible title clock.
    if (c.Enabled(FullComponent::Hardware) &&
        std::tie(cpu, gpu, memory, cpuValue, gpuUsage, memoryValue, packageW, packageNote, cpuW, gpuW, memW) !=
        std::tie(r.cpu, r.gpu, r.memory, r.cpuValue, r.gpuUsage, r.memoryValue, r.packageW, r.packageNote, r.cpuW, r.gpuW, r.memW)) return false;
    if (c.Enabled(FullComponent::Battery) &&
        std::tie(mode, supplemental, low, battery, percent, chip, scheme, saverLabel, etaLabel, heroWord, hero, wattLabel,
                 watt, pphLabel, pph, eta20, foot) !=
        std::tie(r.mode, r.supplemental, r.low, r.battery, r.percent, r.chip, r.scheme, r.saverLabel, r.etaLabel, r.heroWord, r.hero, r.wattLabel,
                 r.watt, r.pphLabel, r.pph, r.eta20, r.foot)) return false;
    if (c.Enabled(FullComponent::PowerChart) &&
        std::tie(mode, powerTrace, powerLabel, powerValue, powerHistory) !=
        std::tie(r.mode, r.powerTrace, r.powerLabel, r.powerValue, r.powerHistory)) return false;
    if (c.Enabled(FullComponent::Network) &&
        std::tie(netState, down, up, totals, latency, netHint) !=
        std::tie(r.netState, r.down, r.up, r.totals, r.latency, r.netHint)) return false;
    if (c.Enabled(FullComponent::Processes) &&
        std::tie(processes, selfCpu) != std::tie(r.processes, r.selfCpu)) return false;
    if (c.Enabled(FullComponent::DiskIO) && std::tie(read, write) != std::tie(r.read, r.write)) return false;
    if (c.Enabled(FullComponent::SystemDisk) &&
        std::tie(disk, diskPercent, diskFree) != std::tie(r.disk, r.diskPercent, r.diskFree)) return false;
    if (c.Enabled(FullComponent::SystemInfo) &&
        std::tie(cpuFrequency, memoryDetail, processCount, uptime) !=
        std::tie(r.cpuFrequency, r.memoryDetail, r.processCount, r.uptime)) return false;
    if (c.Enabled(FullComponent::GpuMemory) &&
        std::tie(sharedMemory, dedicatedMemory, gpuTempVisible, gpuTemp, gpuTempLevel) !=
        std::tie(r.sharedMemory, r.dedicatedMemory, r.gpuTempVisible, r.gpuTemp, r.gpuTempLevel)) return false;
    if (c.Enabled(FullComponent::Wifi) &&
        std::tie(wifi, wifiSsid, wifiSignal) != std::tie(r.wifi, r.wifiSsid, r.wifiSignal)) return false;
    if (c.Enabled(FullComponent::BatteryHealth) &&
        std::tie(health, cycles) != std::tie(r.health, r.cycles)) return false;
    if (c.Enabled(FullComponent::Fps) && std::tie(fps, fpsAdmin) != std::tie(r.fps, r.fpsAdmin)) return false;
    return true;
}

namespace {
std::wstring CompactEtaDuration(double seconds) {
    if (!Known(seconds)) return L"--";
    // Compare only displayed whole minutes, never the changing raw seconds.
    const auto minutes = static_cast<int>(std::min(seconds, 360000.0) / 60);
    if (minutes >= 6000) return L"99+" + T(Key::HourSuffix);
    if (minutes < 60) return std::to_wstring(minutes) + T(Key::MinuteSuffix);
    wchar_t buffer[32]{};
    std::swprintf(buffer, 32, L"%d:%02d", minutes / 60, minutes % 60);
    return buffer;
}
std::wstring CompactSpeed(double bps) {
    if (!Known(bps)) return L"--";
    if (bps == 0) return L"0";
    double value = bps / 1024;
    const std::wstring* suffix = &T(Key::CompactK);
    if (value >= 1024) { value /= 1024; suffix = &T(Key::CompactM); }
    if (value >= 1024) { value /= 1024; suffix = &T(Key::CompactG); }
    return Number(value, value < 100 ? 1 : 0) + *suffix;
}
std::wstring MeterText(MeterStatus status, double watts) {
    if (status == MeterStatus::Valid && std::isfinite(watts) && watts >= 0.0 && watts < 1.0e6) {
        const auto number = Number(watts, watts >= 100 ? 0 : 1);
        // EMI graphics-domain power remains the fallback for unavailable NVML.
        return number + T(Key::WattSuffix);
    }
    switch (status) {
    case MeterStatus::Warmup: return T(Key::MeterWarmup);
    case MeterStatus::CounterUnavailable: return T(Key::MeterNoCounter);
    case MeterStatus::NoMatch: return T(Key::MeterNoDomain);
    case MeterStatus::ReadFailed: return T(Key::MeterReadFailed);
    default: return L"--";
    }
}
bool PositivePower(double watts) { return std::isfinite(watts) && watts > 0.0 && watts < 1.0e6; }
std::wstring Uptime(double seconds) {
    if (!Known(seconds)) return L"--";
    const auto minutes = static_cast<unsigned long long>(std::min(seconds, 3.0e11) / 60);
    std::wstring value;
    if (minutes >= 1440) value = std::to_wstring(minutes / 1440) + T(Key::DayJoin);
    return value + std::to_wstring(minutes / 60 % 24) + T(Key::HourShortJoin) + std::to_wstring(minutes % 60) + T(Key::MinuteSuffix);
}
}
void Model::BreakHistory() {
    std::fill(view_.powerHistory.begin(), view_.powerHistory.end(), -1);
    historyBroken_ = true;
}
bool Model::Update(const Snapshot& s, bool compact, const Components& c, bool taskbar) {
    View v;
    const bool validTemp = std::isfinite(s.gpu_temp_c) && s.gpu_temp_c > 0 && s.gpu_temp_c < 150;
    if (validTemp) gpuTempSeen = true;
    if (compact ? c.Enabled(CompactComponent::GpuTemp) : c.Enabled(FullComponent::GpuMemory)) {
        v.gpuTempVisible = gpuTempSeen;
        if (compact || v.gpuTempVisible) {
            v.gpuTemp = validTemp ? Number(s.gpu_temp_c, 0) + L"°C" : L"--";
            v.gpuTempLevel = validTemp ? (s.gpu_temp_c >= 85 ? 2 : s.gpu_temp_c >= 70 ? 1 : 0) : 0;
        }
    }
    const bool battery = compact ? c.Enabled(CompactComponent::Battery) : c.Enabled(FullComponent::Battery);
    const bool eta = compact && c.Enabled(CompactComponent::Eta);
    const bool power = compact && c.Enabled(CompactComponent::Power);
    const bool chart = !compact && c.Enabled(FullComponent::PowerChart);
    const bool hardware = !compact && c.Enabled(FullComponent::Hardware);
    const bool network = !compact && c.Enabled(FullComponent::Network);
    const bool latency = network || (compact && c.Enabled(CompactComponent::Latency));
    const bool whole = s.power_trace == PowerTrace::BatteryWhole;
    const bool side = s.power_trace == PowerTrace::BatterySide;
    const bool supplemental = s.battery_supplemental && !whole;
    const bool discharging = s.mode == PowerMode::Discharging;
    const bool charging = s.mode == PowerMode::Charging;
    const bool showDischargeEta = whole && discharging && !s.power_contested &&
        s.ac_known && !s.ac_online && (s.battery_rate_valid || (s.eta_from_system && Known(s.eta_s)));
    const bool showChargeEta = charging && s.battery_rate_valid && !supplemental;
    const bool batteryWatts = s.battery_rate_valid && PositivePower(s.power_w);
    if (battery || eta || power || chart) {
        v.mode = s.mode;
        v.supplemental = supplemental;
        v.acOnline = s.ac_known && s.ac_online;
    }
    if (battery || power) v.low = whole && s.percent >= 0 && s.percent <= 20;
    if (battery) {
        v.battery = s.percent < 0 || s.mode == PowerMode::NoBattery ? -1 : Fraction(s.percent);
        v.percent = v.battery < 0 ? L"--" : std::to_wstring(std::clamp(s.percent, 0, 100));
    }
    if (power || chart) {
        // A failed battery read keeps the previous trace so one miss does not join
        // package history to a later battery trace, and does not draw a new point.
        if (s.power_trace == PowerTrace::Unknown && haveSample_ && view_.powerTrace != PowerTrace::Unknown) {
            v.powerTrace = view_.powerTrace;
            v.powerLabel = T(Key::Power);
            v.powerValue = L"--";
        } else if (s.power_trace == PowerTrace::Unknown) {
            v.powerTrace = PowerTrace::Unknown;
            v.powerLabel = T(Key::Power);
            v.powerValue = L"--";
        } else if (whole || side) {
            v.powerTrace = s.power_trace;
            v.powerLabel = whole ? T(Key::System) : T(Key::Battery);
            v.powerValue = batteryWatts ? Number(s.power_w, s.power_w >= 100 ? 0 : 1) : L"--";
        } else {
            v.powerTrace = s.power_trace;
            v.powerLabel = compact ? T(Key::CompactProcessor) : T(Key::Processor);
            v.powerValue = std::isfinite(s.pkg_w) && s.pkg_w >= 0.0 && s.pkg_w < 1.0e6
                ? Number(s.pkg_w, s.pkg_w >= 100 ? 0 : 1) : L"--";
        }
    }
    if (hardware || (compact && c.Enabled(CompactComponent::Cpu))) v.cpuValue = Percent(s.cpu_percent, 0);
    if (hardware || (compact && c.Enabled(CompactComponent::Memory))) v.memoryValue = Percent(s.mem_percent, 0);
    if (hardware || (compact && c.Enabled(CompactComponent::Gpu))) v.gpuUsage = Percent(s.gpu_percent, 0);
    if (latency) {
        v.netState = s.net_state;
        v.latency = s.net_state == NetState::Offline ? T(Key::Offline) :
                    s.net_state == NetState::Checking ? T(Key::Checking) :
                    Known(s.latency_ms) ? Number(s.latency_ms, 0) + T(Key::MsSuffix) : T(Key::UnknownLatency);
    }
    if (compact) {
        if (eta && s.mode != PowerMode::NoBattery) {
            v.compactEtaVisible = true;
            if (supplemental) {
                v.compactEtaValue = T(Key::AssistChip);
            } else if (s.mode == PowerMode::AcIdle && v.acOnline) {
                v.compactEtaValue = s.percent >= 95 ? T(Key::FullyCharged) : T(Key::NotCharging);
            } else if (s.mode == PowerMode::AcIdle) {
                v.compactEtaValue = T(Key::NotCharging);
            } else if (showDischargeEta || showChargeEta) {
                v.compactEtaLabel = showChargeEta ? T(Key::FullIn) : T(Key::TimeLeft);
                v.compactEtaValue = CompactEtaDuration(s.eta_s);
                v.compactEtaFromSystem = showDischargeEta && Known(s.eta_s) && s.eta_from_system;
            } else {
                v.compactEtaValue = L"--";
            }
        }
        if (c.Enabled(CompactComponent::Network)) {
            v.compactUp = L"↑ " + CompactSpeed(s.up_bps);
            v.compactDown = L"↓ " + CompactSpeed(s.down_bps);
            if (taskbar) {
                const auto up = FormatSpeed(s.up_bps), down = FormatSpeed(s.down_bps);
                v.compactUp = up.value + L" " + up.unit;
                v.compactDown = down.value + L" " + down.unit;
            }
        }
        if (c.Enabled(CompactComponent::DiskIO)) {
            v.diskRead = T(Key::ReadPrefix) + CompactSpeed(s.disk_read_bps);
            v.diskWrite = T(Key::WritePrefix) + CompactSpeed(s.disk_write_bps);
            if (taskbar) {
                const auto read = FormatSpeed(s.disk_read_bps), write = FormatSpeed(s.disk_write_bps);
                v.diskRead = read.value + L" " + read.unit;
                v.diskWrite = write.value + L" " + write.unit;
            }
        }
        if (c.Enabled(CompactComponent::CpuFrequency)) v.cpuFrequency = Number(s.cpu_mhz / 1000, 2) + T(Key::GhzSuffix);
        if (c.Enabled(CompactComponent::Fps)) {
            v.fpsAdmin = s.fps_status == 2;
            if (v.fpsAdmin) v.fps = T(Key::RunAsAdmin);
            else if (s.fps_status == 1 && Known(s.fps)) v.fps = Number(s.fps, 0);
            else v.fps = L"--";
        }
    } else {
        if (hardware) {
            v.cpu = Fraction(s.cpu_percent); v.gpu = Fraction(s.gpu_percent); v.memory = Fraction(s.mem_percent);
            const bool packageKnown = s.pkg_meter == MeterStatus::Valid && std::isfinite(s.pkg_w) && s.pkg_w >= 0.0 && s.pkg_w < 1.0e6;
            v.packageW = packageKnown ? Number(s.pkg_w, s.pkg_w >= 100 ? 0 : 1) : L"--";
            v.packageNote = packageKnown ? T(Key::WholeChip) : MeterText(s.pkg_meter, NAN);
            v.cpuW = MeterText(s.cpu_meter, s.cpu_w);
            v.gpuW = s.dgpu_status == DgpuStatus::Valid && PositivePower(s.dgpu_w)
                ? T(Key::DgpuPrefix) + Number(s.dgpu_w, s.dgpu_w >= 10 ? 0 : 1) + T(Key::WattSuffix)
                : s.dgpu_status == DgpuStatus::Sleeping ? T(Key::DgpuSleeping)
                : s.dgpu_status == DgpuStatus::Glitch ? T(Key::DgpuPrefix) + L"--"
                : MeterText(s.gpu_meter, s.gpu_w);
            v.memW = MeterText(s.mem_meter, s.mem_w);
        }
        if (battery) {
            v.chip = v.low ? T(Key::LowBattery) : supplemental ? T(Key::AssistChip) :
                     discharging ? T(Key::Discharging) : charging ? T(Key::Charging) :
                     s.mode == PowerMode::AcIdle && v.acOnline ? T(Key::OnAc) :
                     s.mode == PowerMode::AcIdle ? T(Key::NotCharging) :
                     s.mode == PowerMode::NoBattery ? T(Key::NoBattery) : T(Key::Reading);
            const std::wstring plan = s.active_plan_valid && !s.active_plan_name.empty()
                ? s.active_plan_name : T(Key::PowerPlanUnknown);
            Key overlay = Key::Balanced;
            switch (s.power_scheme_kind) {
            case PowerSchemeKind::BestPerformance: overlay = Key::BestPerformance; break;
            case PowerSchemeKind::BestEfficiency: overlay = Key::BestEfficiency; break;
            case PowerSchemeKind::LongerBattery: overlay = Key::LongerBattery; break;
            default: break;
            }
            v.scheme = s.active_plan_valid && s.active_plan_balanced && overlay != Key::Balanced
                ? T(Key::PowerModePrefix) + T(overlay) : T(Key::PowerPlanPrefix) + plan;
            v.batterySaver = s.battery_saver;
            v.saverLabel = s.battery_saver ? T(Key::BatterySaverShort) : L"";
            v.etaLabel = T(Key::BatteryStatus);
            if (supplemental) {
                v.heroWord = T(Key::ExternalDischarge);
            } else if (showDischargeEta || showChargeEta) {
                v.etaLabel = showDischargeEta ? T(Key::EtaLeft) : T(Key::EtaFull);
                if (showDischargeEta && s.eta_from_system && Known(s.eta_s)) v.etaLabel += T(Key::SystemEstimate);
                v.hero = Parts(showDischargeEta || showChargeEta ? s.eta_s : NAN);
                if (v.hero.empty()) v.heroWord = T(Key::Calculating);
            } else if (s.mode == PowerMode::AcIdle && v.acOnline) {
                v.heroWord = s.percent >= 99 ? T(Key::FullyCharged) : T(Key::AcPower);
            } else if (s.mode == PowerMode::AcIdle) {
                v.heroWord = T(Key::NotCharging);
            } else if (s.mode == PowerMode::NoBattery) {
                v.heroWord = T(Key::NoBatteryDetected);
            } else if (discharging) {
                v.heroWord = L"--";
            } else {
                v.heroWord = T(Key::Reading);
            }
            if (charging) v.wattLabel = T(Key::ChargePower);
            else if (supplemental) v.wattLabel = T(Key::NetDischarge);
            else if (whole) v.wattLabel = T(Key::LivePower);
            else if (side) v.wattLabel = T(Key::BatteryPower);
            else if (s.mode == PowerMode::NoBattery) v.wattLabel = T(Key::Power);
            else v.wattLabel = T(Key::BatteryPower);
            v.watt = batteryWatts && (charging || supplemental || whole || side)
                ? Number(s.power_w, s.power_w >= 100 ? 0 : 1) : L"--";
            if (showChargeEta || showDischargeEta) {
                v.pphLabel = showChargeEta ? T(Key::ChargePerHour) : T(Key::DrainPerHour);
                v.pph = Number(s.pct_per_hour, s.pct_per_hour >= 10 ? 0 : 1);
                v.eta20 = showDischargeEta && Known(s.eta20_s) ? ShortDuration(s.eta20_s) : L"";
            } else {
                v.pphLabel = T(Key::PerHour);
                v.pph = L"--";
            }
            if (whole && Known(s.on_battery_s)) {
                v.foot = (s.on_battery_since_start ? T(Key::SinceLaunch) : T(Key::SinceUnplugged)) + Duration(s.on_battery_s);
                if (Known(s.avg_w)) v.foot += T(Key::AveragePrefix) + Number(s.avg_w) + T(Key::WattSuffix);
            } else if (Known(s.remaining_wh) && Known(s.full_wh) && s.full_wh > 0)
                v.foot = T(Key::RemainingPrefix) + Number(s.remaining_wh) + T(Key::FullCapacityPrefix) + Number(s.full_wh) + T(Key::WhSuffix);
        }
        if (chart) {
            if (s.power_trace == PowerTrace::Unknown) {
                if (view_.powerHistory.size() == 120) v.powerHistory = view_.powerHistory;
            } else {
                const bool sameSource = haveSample_ && !historyBroken_ && !compact_ &&
                    components_.Enabled(FullComponent::PowerChart) &&
                    view_.powerTrace == s.power_trace && view_.powerHistory.size() == 120;
                if (sameSource) v.powerHistory = view_.powerHistory;
                else v.powerHistory.assign(120, -1);
                std::move(v.powerHistory.begin() + 1, v.powerHistory.end(), v.powerHistory.begin());
                const double point = (whole || side) ? s.power_w : s.pkg_w;
                v.powerHistory.back() = ChartValue(point);
            }
        }
        if (network) {
            v.down = FormatSpeed(s.down_bps); v.up = FormatSpeed(s.up_bps);
            v.totals = T(Key::SessionTraffic) + Bytes(s.rx_total) + L" · ↑ " + Bytes(s.tx_total);
            if (s.net_state == NetState::Offline) {
                v.netHint = OfflineReasonText(s.offline_reason, s.offline_latency_ms);
                if (s.offline_since_unix > 0) {
                    const auto elapsed = std::max(0.0, std::difftime(std::time(nullptr), static_cast<std::time_t>(s.offline_since_unix)));
                    v.netHint = T(Key::DownFor) + OutageDuration(static_cast<std::uint64_t>(elapsed)) + L" · " + v.netHint;
                }
            } else if (s.net_state == NetState::Slow) v.netHint = T(Key::SlowConnected);
            else if (s.net_state == NetState::Online) v.netHint = T(Key::NetworkOk);
            else v.netHint = T(Key::CheckingConnection);
        }
        if (c.Enabled(FullComponent::Processes)) {
            v.selfCpu = Known(s.self_cpu_percent) ? T(Key::SelfCpu) + Percent(s.self_cpu_percent, 1) : L"";
            const double top = !s.top_procs.empty() && Known(s.top_procs.front().cpu_percent)
                                   ? std::max(s.top_procs.front().cpu_percent, 5.0) : 5.0;
            for (size_t i = 0; i < std::min(size_t(3), s.top_procs.size()); ++i) {
                const auto& p = s.top_procs[i];
                v.processes.push_back({p.name.empty() ? L"--" : p.name, Percent(p.cpu_percent, 1),
                    Known(p.cpu_percent) ? static_cast<float>(std::clamp(p.cpu_percent / top, 0.0, 1.0)) : 0.0f});
            }
        }
        if (c.Enabled(FullComponent::DiskIO)) { v.read = FormatSpeed(s.disk_read_bps); v.write = FormatSpeed(s.disk_write_bps); }
        if (c.Enabled(FullComponent::SystemDisk)) {
            v.disk = Fraction(s.sys_disk_used_percent); v.diskPercent = Percent(s.sys_disk_used_percent, 0);
            v.diskFree = Number(s.sys_disk_free_gb, 1) + T(Key::GbSuffix);
        }
        if (c.Enabled(FullComponent::SystemInfo)) {
            v.cpuFrequency = Number(s.cpu_mhz / 1000, 2) + T(Key::GhzSuffix);
            v.memoryDetail = Number(s.mem_used_gb, 1) + L" / " + Number(s.mem_total_gb, 1) + T(Key::GbSuffix);
            v.processCount = s.process_count < 0 ? L"--" : std::to_wstring(s.process_count);
            v.uptime = Uptime(s.uptime_s);
        }
        if (c.Enabled(FullComponent::GpuMemory)) {
            v.sharedMemory = Number(s.gpu_shared_mem_mb / 1024, 2) + T(Key::GbSuffix);
            v.dedicatedMemory = Number(s.gpu_dedicated_mem_mb / 1024, 2) + T(Key::GbSuffix);
        }
        if (c.Enabled(FullComponent::Wifi)) {
            v.wifiSsid = s.wifi_ssid.empty() ? L"--" : s.wifi_ssid;
            v.wifi = s.wifi_signal < 0 ? -1 : Fraction(s.wifi_signal);
            v.wifiSignal = s.wifi_signal < 0 ? L"--" : Percent(s.wifi_signal, 0);
        }
        if (c.Enabled(FullComponent::BatteryHealth)) {
            v.health = Known(s.battery_health_percent) ? Number(s.battery_health_percent, 1) + L"%" : L"--";
            v.cycles = s.battery_cycles < 0 ? L"--" : std::to_wstring(s.battery_cycles);
        }
        if (c.Enabled(FullComponent::Fps)) {
            v.fpsAdmin = s.fps_status == 2;
            if (v.fpsAdmin) v.fps = T(Key::AdminRequired);
            else if (s.fps_status == 1 && Known(s.fps)) {
                v.fps = Number(s.fps, 0) + T(Key::FpsSuffix);
                if (!s.fps_process.empty()) v.fps += L" · " + s.fps_process;
            } else v.fps = L"--";
        }
        std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_MSC_VER)
        localtime_s(&local, &now);
#else
        if (const auto* t = std::localtime(&now)) local = *t;
#endif
        wchar_t clock[16]{};
        std::swprintf(clock, 16, L"%02d:%02d", local.tm_hour, local.tm_min);
        v.clock = clock;
    }
    const bool selectionChanged = compact_ != compact ||
        (compact ? components_.compact != c.compact : components_.full != c.full);
    const bool changed = !haveSample_ || historyBroken_ || selectionChanged || !v.Equals(view_, compact, c);
    view_ = std::move(v); components_ = c; compact_ = compact;
    haveSample_ = true; historyBroken_ = false;
    return changed;
}
} // namespace pulse::ui
