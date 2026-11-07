#include "metrics_mac.h"
#include "netprobe_mac.h"
#include "smc_mac.h"
#include "mac_strings.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::mutex print_mutex;
void Print(const std::wstring& text) {
    std::string bytes;
    @autoreleasepool { bytes = pulse::mac::Utf8(text); }
    std::lock_guard<std::mutex> lock(print_mutex);
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}
const wchar_t* Name(pulse::PowerMode mode) {
    switch (mode) {
    case pulse::PowerMode::Unknown: return L"Unknown";
    case pulse::PowerMode::NoBattery: return L"NoBattery";
    case pulse::PowerMode::Discharging: return L"Discharging";
    case pulse::PowerMode::Charging: return L"Charging";
    case pulse::PowerMode::AcIdle: return L"AcIdle";
    }
    return L"Unknown";
}
const wchar_t* Name(pulse::PowerTrace trace) {
    switch (trace) {
    case pulse::PowerTrace::Unknown: return L"Unknown";
    case pulse::PowerTrace::Package: return L"Package";
    case pulse::PowerTrace::BatteryWhole: return L"BatteryWhole";
    case pulse::PowerTrace::BatterySide: return L"BatterySide";
    }
    return L"Unknown";
}
const wchar_t* Name(pulse::MeterStatus status) {
    switch (status) {
    case pulse::MeterStatus::Off: return L"Off";
    case pulse::MeterStatus::Warmup: return L"Warmup";
    case pulse::MeterStatus::CounterUnavailable: return L"CounterUnavailable";
    case pulse::MeterStatus::NoMatch: return L"NoMatch";
    case pulse::MeterStatus::ReadFailed: return L"ReadFailed";
    case pulse::MeterStatus::Valid: return L"Valid";
    }
    return L"Unknown";
}
const wchar_t* Name(pulse::NetState state) {
    switch (state) {
    case pulse::NetState::Checking: return L"Checking";
    case pulse::NetState::Online: return L"Online";
    case pulse::NetState::Slow: return L"Slow";
    case pulse::NetState::Offline: return L"Offline";
    }
    return L"Unknown";
}
const wchar_t* Name(pulse::OfflineReason reason) {
    switch (reason) {
    case pulse::OfflineReason::None: return L"None";
    case pulse::OfflineReason::Timeout: return L"Timeout";
    case pulse::OfflineReason::HighLatency: return L"HighLatency";
    case pulse::OfflineReason::LinkDisconnected: return L"LinkDisconnected";
    case pulse::OfflineReason::NoResponse: return L"NoResponse";
    }
    return L"Unknown";
}
const wchar_t* Name(pulse::PowerSchemeKind kind) {
    switch (kind) {
    case pulse::PowerSchemeKind::Unknown: return L"Unknown";
    case pulse::PowerSchemeKind::Balanced: return L"Balanced";
    case pulse::PowerSchemeKind::BestPerformance: return L"BestPerformance";
    case pulse::PowerSchemeKind::BestEfficiency: return L"BestEfficiency";
    case pulse::PowerSchemeKind::LongerBattery: return L"LongerBattery";
    }
    return L"Unknown";
}
std::wstring Value(double value) {
    if (!std::isfinite(value)) return L"--";
    std::wostringstream out; out << std::fixed << std::setprecision(4) << value; return out.str();
}
void Field(std::wostream& out, const char* name, double value) { out << pulse::mac::FromUtf8(name) << L"=" << Value(value) << L"\n"; }
void Field(std::wostream& out, const char* name, int value) {
    out << pulse::mac::FromUtf8(name) << L"=" << (value < 0 ? L"--" : std::to_wstring(value)) << L"\n";
}
void Field(std::wostream& out, const char* name, bool value) { out << pulse::mac::FromUtf8(name) << L"=" << (value ? L"true" : L"false") << L"\n"; }
void Field(std::wostream& out, const char* name, const std::wstring& value) {
    out << pulse::mac::FromUtf8(name) << L"=" << (value.empty() ? L"--" : value) << L"\n";
}
void Field(std::wostream& out, const char* name, const wchar_t* value) { Field(out, name, std::wstring(value)); }
void Field(std::wostream& out, const char* name, std::uint64_t value) { out << pulse::mac::FromUtf8(name) << L"=" << value << L"\n"; }
void Field(std::wostream& out, const char* name, std::int64_t value) { out << pulse::mac::FromUtf8(name) << L"=" << value << L"\n"; }

void SnapshotText(std::wostream& out, const pulse::Snapshot& s) {
#define FIELD(n) Field(out, #n, s.n)
#define ENUM(n) Field(out, #n, Name(s.n))
    ENUM(mode); FIELD(percent); FIELD(power_w); FIELD(eta_s); FIELD(eta_from_system);
    FIELD(eta20_s); FIELD(pct_per_hour); FIELD(on_battery_s); FIELD(on_battery_since_start);
    FIELD(avg_w); FIELD(remaining_wh); FIELD(full_wh); FIELD(battery_saver); FIELD(power_scheme);
    FIELD(pkg_w); FIELD(cpu_w); FIELD(gpu_w); FIELD(mem_w); FIELD(gpu_percent); FIELD(system_w);
    FIELD(down_bps); FIELD(up_bps); FIELD(rx_total); FIELD(tx_total); ENUM(net_state); FIELD(latency_ms);
    FIELD(offline_since_unix); FIELD(offline_by_latency); FIELD(cpu_percent); FIELD(mem_percent);
    out << L"top_procs=" << s.top_procs.size() << L"\n";
    for (std::size_t i = 0; i < s.top_procs.size(); ++i)
        out << L"top" << i << L"_name=" << s.top_procs[i].name << L" CPU=" << Value(s.top_procs[i].cpu_percent) << L"\n";
    FIELD(self_cpu_percent); FIELD(mem_used_gb); FIELD(mem_total_gb); FIELD(disk_read_bps); FIELD(disk_write_bps);
    FIELD(sys_disk_used_percent); FIELD(sys_disk_free_gb); FIELD(cpu_mhz); FIELD(process_count); FIELD(uptime_s);
    FIELD(gpu_shared_mem_mb); FIELD(gpu_dedicated_mem_mb); FIELD(wifi_ssid); FIELD(wifi_signal);
    FIELD(battery_health_percent); FIELD(battery_cycles); FIELD(fps); FIELD(fps_status); FIELD(fps_process);
    out << L"fps_top=" << s.fps_top.size() << L"\n";
    for (const auto& rank : s.fps_top) out << L"fps_rank name=" << rank.name << L" pid=" << rank.pid << L" frames=" << rank.frames << L"\n";
    FIELD(pkg_power_from_energy); FIELD(pkg_energy_delta_pwh); FIELD(pkg_energy_interval_s); FIELD(pkg_power_counter_w);
    FIELD(cpu_time_percent); FIELD(cpu_utility_percent); FIELD(cpu_from_utility); ENUM(power_scheme_kind);
    ENUM(offline_reason); FIELD(offline_latency_ms); FIELD(gpu_temp_c); FIELD(active_plan_valid); FIELD(active_plan_name);
    FIELD(ac_known); FIELD(ac_online); FIELD(battery_rate_valid); FIELD(battery_rate_mw); FIELD(battery_supplemental);
    FIELD(power_contested); ENUM(power_trace); ENUM(pkg_meter); ENUM(cpu_meter); ENUM(gpu_meter); ENUM(mem_meter);
    FIELD(active_plan_balanced); Field(out, "dgpu_status", pulse::DgpuStatusName(s.dgpu_status)); FIELD(dgpu_w);
#undef ENUM
#undef FIELD
}
struct Timings {
    static constexpr std::array<const char*, 14> names = {{"Battery", "Battery registry", "Battery IOPS", "IOReport", "IOReport sample", "IOReport parse", "CPU", "Memory", "Process scan", "GPU", "Network throughput", "Disk IO", "System disk", "Sample total"}};
    std::array<double, names.size()> milliseconds;
    void Reset() { milliseconds.fill(NAN); }
    static void Receive(const char* stage, double ms, void* user) {
        auto& self = *static_cast<Timings*>(user);
        for (std::size_t i = 0; i < names.size(); ++i)
            if (std::strcmp(stage, names[i]) == 0) { self.milliseconds[i] = ms; break; }
    }
    void Text(std::wostream& out) const {
        out << L"timing_ms:";
        for (std::size_t i = 0; i < names.size(); ++i) out << L" " << pulse::mac::FromUtf8(names[i]) << L"=" << Value(milliseconds[i]);
        out << L"（-- 表示未采集）\n";
    }
};
void OnNet(const pulse::NetEvent& event, void*) {
    @autoreleasepool {
        std::wostringstream out;
        out << L"NET recovered=" << event.recovered << L" reason=" << Name(event.reason)
            << L" latency_ms=" << Value(event.latency_ms) << L" offline_start_unix=" << event.offline_start_unix
            << L" event_unix=" << event.event_unix << L" outage_duration_s=" << event.outage_duration_s
            << L" msg=" << event.message << L"\n";
        Print(out.str());
    }
}
int Integer(const char* text, int minimum, int maximum) {
    errno = 0; char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    return errno || end == text || *end || value < minimum || value > maximum ? -1 : static_cast<int>(value);
}
int Usage() {
    Print(L"用法: metrics_selftest [--samples N] [--interval-ms N] [--compact] [--no-probe | --require-proxy] [--brief] [--quiet]\n"
          L"                      [--power | --net | --procs | --gpu | --telemetry [5–600] | --mem | --smc [5–600]] [--slow-ms N] [--url URL ...]\n");
    return 2;
}
int Main(int argc, char* argv[]) {
    int samples = 5, interval_ms = 0;
    bool full = true, brief = false, quiet = false, no_probe = false, require_proxy = false;
    int diagnostic = 0, telemetry_samples = 30, smc_samples = 30;
    pulse::Settings settings;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--compact") full = false;
        else if (arg == "--brief") brief = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--no-probe") no_probe = true;
        else if (arg == "--require-proxy") require_proxy = true;
        else if (arg == "--telemetry") {
            if (diagnostic) return Usage();
            diagnostic = 5;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                telemetry_samples = Integer(argv[++i], 5, 600);
                if (telemetry_samples < 0) return Usage();
            }
        } else if (arg == "--smc") {
            if (diagnostic) return Usage();
            diagnostic = 7;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                smc_samples = Integer(argv[++i], 5, 600);
                if (smc_samples < 0) return Usage();
            }
        } else if (arg == "--mem") {
            if (diagnostic) return Usage();
            diagnostic = 6;
        } else if (arg == "--power" || arg == "--net" || arg == "--procs" || arg == "--gpu") {
            if (diagnostic) return Usage();
            diagnostic = arg == "--power" ? 1 : arg == "--net" ? 2 : arg == "--procs" ? 3 : 4;
        } else if (arg == "--samples" || arg == "--interval-ms" || arg == "--slow-ms") {
            if (++i >= argc) return Usage();
            const int value = Integer(argv[i], 1, arg == "--samples" ? 86400 : 60000);
            if (value < 0) return Usage();
            if (arg == "--samples") samples = value;
            else if (arg == "--interval-ms") interval_ms = value;
            else { if (value >= settings.unusable_ms) return Usage(); settings.slow_ms = value; }
        } else if (arg == "--url") {
            if (++i >= argc) return Usage();
            settings.probe_urls.push_back(pulse::mac::FromUtf8(argv[i]));
        } else return Usage();
    }
    if (no_probe && require_proxy) return Usage();
    pulse::mac::SetProbeRequireProxy(require_proxy);
    if (diagnostic) {
        if (diagnostic == 5) { pulse::mac::RunTelemetryDiagnostics(telemetry_samples, Print); return 0; }
        if (diagnostic == 7) { pulse::mac::RunSmcDiagnostics(smc_samples, Print); return 0; }
        if (diagnostic == 6) { pulse::mac::RunMemoryDiagnostics(Print); return 0; }
        if (diagnostic == 2 && no_probe) return Usage();
        Print(diagnostic == 1 ? pulse::BuildPowerDiagnosticsReport() : diagnostic == 2 ?
            pulse::mac::BuildNetworkDiagnosticsReport(settings) : diagnostic == 3 ?
            pulse::mac::BuildProcessDiagnosticsReport() : pulse::mac::BuildGpuDiagnosticsReport());
        return 0;
    }
    if (!interval_ms) interval_ms = full ? 1000 : 2000;
    Timings timings; timings.Reset();
    if (!quiet) pulse::mac::SetTimingCallback(Timings::Receive, &timings);
    const auto start = Clock::now();
    const double cpu_start = pulse::mac::SelfCpuSeconds();
    std::vector<double> durations; durations.reserve(static_cast<std::size_t>(samples));
    {
        pulse::Metrics metrics;
        pulse::PowerTrace trace = pulse::PowerTrace::Unknown;
        pulse::Snapshot previous_snapshot;
        metrics.SetProbeEnabled(!no_probe);
        metrics.Start(settings, quiet ? nullptr : OnNet, nullptr);
        if (!quiet) Print(L"macOS 阶段 0；未知=--；进程 CPU 占全部核心；IOReport 每次真实差分；首拍预热。\n");
        auto due = Clock::now();
        for (int i = 0; i < samples; ++i) {
            std::this_thread::sleep_until(due);
            pulse::Wants wants;
            wants.chip_power = full || (pulse::NeedsPackagePower(trace) &&
                !pulse::mac::CanSkipCompactPackagePower(previous_snapshot));
            wants.gpu_usage = wants.processes = full;
            wants.memory_detail = wants.disk_io = wants.sys_disk = wants.uptime = wants.battery_health = wants.process_count = full;
            metrics.SetWants(wants);
            timings.Reset();
            const auto before = Clock::now();
            const auto snapshot = metrics.Sample(full);
            previous_snapshot = snapshot;
            const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
            durations.push_back(milliseconds);
            trace = pulse::NextCollectionTrace(trace, snapshot.power_trace);
            if (!quiet) {
                std::wostringstream out;
                if (brief) out << L"sample=" << i + 1 << L" mode=" << Name(snapshot.mode) << L" battery="
                    << (snapshot.percent < 0 ? L"--" : std::to_wstring(snapshot.percent))
                    << L"% W=" << Value(snapshot.power_w) << L" CPU=" << Value(snapshot.cpu_percent)
                    << L" MEM=" << Value(snapshot.mem_percent) << L" net=" << Name(snapshot.net_state)
                    << L" latency_ms=" << Value(snapshot.latency_ms) << L" Sample_ms=" << Value(milliseconds) << L"\n";
                else { out << L"--- sample " << i + 1 << L" ---\n"; SnapshotText(out, snapshot); timings.Text(out); }
                out << L"battery_power_source=" << pulse::mac::LastBatteryPowerSource() << L"\n"
                    << L"system_w=" << Value(snapshot.system_w) << L" system_power_source=" << pulse::mac::LastSystemPowerSource() << L"\n";
                Print(out.str());
            }
            due += std::chrono::milliseconds(interval_ms);
            // Don't burst samples to catch up after suspend or a very slow collector.
            if (due < Clock::now()) due = Clock::now() + std::chrono::milliseconds(interval_ms);
        }
    } // Includes joining our worker and scheduling NSURLSession cancellation.
    const double cpu_seconds = pulse::mac::SelfCpuSeconds() - cpu_start;
    const double wall_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    pulse::mac::SetTimingCallback(nullptr, nullptr);
    std::wostringstream out;
    out << L"总时长_s=" << Value(wall_seconds) << L" 本进程CPU_s=" << Value(cpu_seconds)
        << L" 单核CPU%=" << Value(wall_seconds > 0 ? 100.0 * cpu_seconds / wall_seconds : NAN) << L"\n";
    if (!quiet) {
        std::sort(durations.begin(), durations.end());
        const auto n = durations.size();
        const double median = n % 2 ? durations[n / 2] : (durations[n / 2 - 1] + durations[n / 2]) / 2;
        out << L"Sample_ms median=" << Value(median) << L" max=" << Value(durations.back())
            << L"（含首拍；CPU 总计含初始化、网络、打印和退出）\n";
    }
    Print(out.str()); return 0;
}
} // anonymous namespace
int main(int argc, char* argv[]) { @autoreleasepool { return Main(argc, argv); } }
