#pragma once
#include "metrics.h"
#include "i18n.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::ui {
using i18n::Key;
using i18n::T;

enum class DisplayMode { Full, Compact, Taskbar };

enum class FullComponent : std::size_t {
    Hardware, Battery, PowerChart, Network, Processes, DiskIO, SystemDisk,
    SystemInfo, GpuMemory, Wifi, BatteryHealth, Fps, Count
};
enum class CompactComponent : std::size_t {
    Battery, Eta, Power, Cpu, Memory, Gpu, Network, Latency, DiskIO, CpuFrequency, Fps, GpuTemp, Count
};
struct ComponentInfo {
    const wchar_t* key;
    Key label;
    bool enabled;
    float extent; // Full: card height; compact: minimum cell width, in DIPs.
};
inline constexpr std::array<ComponentInfo, static_cast<std::size_t>(FullComponent::Count)> FullComponents{{
    {L"hardware", Key::HardwareItems, true, 96},
    {L"battery", Key::BatteryItems, true, 156},
    {L"power_chart", Key::PowerChartItems, false, 82},
    {L"network", Key::Network, true, 154},
    {L"processes", Key::ProcessesItems, true, 98},
    {L"disk_io", Key::DiskIOItems, false, 78},
    {L"sys_disk", Key::SystemDiskItems, false, 84},
    {L"system_info", Key::SystemInfo, false, 114},
    {L"gpu_memory", Key::GpuMemory, false, 84},
    {L"wifi", Key::Wifi, false, 80},
    {L"battery_health", Key::BatteryHealth, false, 78},
    {L"fps", Key::Fps, false, 72}
}};
inline constexpr std::array<ComponentInfo, static_cast<std::size_t>(CompactComponent::Count)> CompactComponents{{
    {L"battery", Key::BatteryPercent, true, 68},
    {L"eta", Key::EtaItems, true, 0},
    {L"power", Key::TotalPower, true, 89},
    {L"cpu", Key::CpuPercent, true, 64},
    {L"memory", Key::MemoryPercent, true, 64},
    {L"gpu", Key::GpuPercent, false, 64},
    {L"network", Key::NetworkSpeed, true, 0},
    {L"latency", Key::Latency, true, 75},
    {L"disk_io", Key::DiskIO, false, 0},
    {L"cpu_freq", Key::CpuFrequency, false, 89},
    {L"fps", Key::FpsShort, false, 57},
    {L"gpu_temp", Key::GpuTemp, false, 64}
}};
inline constexpr std::array<FullComponent, FullComponents.size()> FullOrder{{
    FullComponent::Hardware, FullComponent::Battery, FullComponent::PowerChart,
    FullComponent::Network, FullComponent::DiskIO, FullComponent::SystemDisk,
    FullComponent::SystemInfo, FullComponent::GpuMemory, FullComponent::Wifi,
    FullComponent::BatteryHealth, FullComponent::Fps, FullComponent::Processes
}};
inline constexpr auto TaskbarComponents = [] {
    auto items = CompactComponents;
    for (auto& item : items) item.enabled = false;
    for (auto c : {CompactComponent::Network, CompactComponent::Cpu, CompactComponent::Memory,
                   CompactComponent::Eta, CompactComponent::Power, CompactComponent::Latency})
        items[static_cast<std::size_t>(c)].enabled = true;
    return items;
}();
struct Components {
    std::array<bool, FullComponents.size()> full{};
    std::array<bool, CompactComponents.size()> compact{};
    std::array<bool, CompactComponents.size()> taskbar{};
    Components() { Reset(); }
    void Reset();
    bool Enabled(FullComponent c) const { return full[static_cast<std::size_t>(c)]; }
    bool Enabled(CompactComponent c) const { return compact[static_cast<std::size_t>(c)]; }
};
Wants CollectionWants(const Components& components, DisplayMode mode, bool details = false,
                      PowerTrace trace = PowerTrace::Unknown);

struct Speed {
    std::wstring value, unit;
    bool operator==(const Speed& r) const { return value == r.value && unit == r.unit; }
};
struct TimePart {
    std::wstring value, unit;
    bool operator==(const TimePart& r) const { return value == r.value && unit == r.unit; }
};
struct Process {
    std::wstring name, percent;
    float fraction = 0;
    bool operator==(const Process& r) const {
        return name == r.name && percent == r.percent && fraction == r.fraction;
    }
};

// Canonical display values. Only enabled components are populated or compared.
struct View {
    PowerMode mode = PowerMode::Unknown;
    PowerTrace powerTrace = PowerTrace::Unknown;
    NetState netState = NetState::Checking;
    bool low = false;
    bool supplemental = false;
    bool acOnline = false;
    bool batterySaver = false;
    bool compactEtaVisible = false, compactEtaFromSystem = false;
    float battery = -1, cpu = -1, gpu = -1, memory = -1, disk = -1, wifi = -1;
    std::wstring clock, chip, scheme, saverLabel, percent, etaLabel, heroWord;
    std::vector<TimePart> hero;
    std::wstring wattLabel, watt, pphLabel, pph, eta20, foot;
    std::wstring totals, selfCpu, powerLabel, powerValue; // Shared by compact power and the chart.
    std::wstring compactEtaLabel, compactEtaValue;
    std::wstring cpuValue, memoryValue, packageW, packageNote, cpuW, gpuW, memW, gpuUsage;
    std::wstring latency, netHint, compactUp, compactDown, diskRead, diskWrite;
    std::wstring diskPercent, diskFree, cpuFrequency, memoryDetail, processCount, uptime;
    std::wstring sharedMemory, dedicatedMemory, wifiSsid, wifiSignal, health, cycles, fps;
    bool fpsAdmin = false;
    std::wstring gpuTemp;
    bool gpuTempVisible = false; // Any UI model has seen a valid value in this run.
    int gpuTempLevel = 0; // 0 normal/unknown, 1 >= 70 C, 2 >= 85 C.
    Speed down, up, read, write;
    std::vector<Process> processes;
    std::vector<float> powerHistory; // Allocated only while the chart is visible.
    bool Equals(const View& r, bool compact, const Components& components) const;
};

class Model {
public:
    bool Update(const Snapshot& snapshot, bool compact, const Components& components, bool taskbar = false);
    void BreakHistory();
    const View& Data() const { return view_; }
private:
    View view_;
    Components components_;
    bool compact_ = false, haveSample_ = false, historyBroken_ = false;
};

std::wstring Number(double value, int decimals = 1);
std::wstring Duration(double seconds);
std::wstring ShortDuration(double seconds);
std::wstring OutageDuration(std::uint64_t seconds);
std::wstring OfflineReasonText(OfflineReason reason, double latencyMs);
std::wstring NetworkEventText(const NetEvent& event);
} // namespace pulse::ui
