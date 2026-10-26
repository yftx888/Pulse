#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "i18n.h"
#include <windows.h>
#include <cwchar>

namespace pulse::i18n {
namespace {
struct Entry { std::wstring zh, en; };
// The two columns have exactly the same order as Key in i18n.h.
const Entry strings[] = {
    {L"Pulse", L"Pulse"}, // AppName
    {L"Pulse 启动失败，请检查系统图形组件与可用内存。", L"Pulse could not start. Check the system graphics components and available memory."}, // StartupFailed
    {L"Pulse 遇到无法恢复的错误。", L"Pulse encountered an unrecoverable error."}, // FatalError
    {L"无法初始化 Direct2D / DirectWrite。", L"Could not initialize Direct2D / DirectWrite."}, // GraphicsFailed
    {L"无法更新当前用户的开机自启设置。", L"Could not update the startup setting for the current user."}, // StartupSettingFailed
    {L"日志路径尚未就绪。", L"The log path is not ready yet."}, // LogNotReady
    {L"无法打开日志，日志文件可能尚未生成。", L"Could not open the log. The file may not have been created yet."}, // LogOpenFailed
    {L"Pulse · 网络提醒", L"Pulse · Network alert"}, // ToastCaption
    {L"Pulse · 双击窗口切换模式", L"Pulse · Double-click the window to switch modes"}, // TrayTip
    {L"语言 / Language", L"语言 / Language"}, // LanguageMenu
    {L"跟随系统", L"Follow system"}, // LanguageAuto
    {L"中文", L"中文"}, // LanguageZh
    {L"English", L"English"}, // LanguageEn
    {L"完整模式", L"Full mode"}, // FullMode
    {L"迷你模式", L"Mini mode"}, // MiniMode
    {L"界面大小", L"Size"}, // Size
    {L"界面大小（自定义 {0}%）", L"Size (custom {0}%)"}, // CustomSize
    {L"（默认）", L" (default)"}, // DefaultSuffix
    {L"刷新间隔", L"Refresh interval"}, // RefreshInterval
    {L"延迟较慢阈值", L"Slow latency threshold"}, // SlowThreshold
    {L"断网提醒", L"Disconnect alerts"}, // DisconnectAlerts
    {L"提醒提示音", L"Alert sound"}, // AlertSound
    {L"显示内容", L"Show items"}, // ShowItems
    {L"窗口置顶", L"Always on top"}, // AlwaysOnTop
    {L"开机自启", L"Start with Windows"}, // StartWithWindows
    {L"创建快捷方式", L"Create shortcut"}, // CreateShortcut
    {L"桌面", L"Desktop"}, // ShortcutDesktop
    {L"开始菜单", L"Start menu"}, // ShortcutStartMenu
    {L"快捷方式已存在", L"The shortcut already exists."}, // ShortcutExists
    {L"已有指向其他位置的 Pulse 快捷方式，是否替换？", L"A Pulse shortcut pointing to another location already exists. Replace it?"}, // ShortcutReplace
    {L"Pulse 当前在临时/下载目录运行，移动或删除后快捷方式会失效，仍要创建吗？", L"Pulse is running from a temporary or Downloads folder. Moving or deleting it will break the shortcut. Create it anyway?"}, // ShortcutLocationWarning
    {L"快捷方式已创建", L"Shortcut created."}, // ShortcutCreated
    {L"创建快捷方式失败（错误码：{0}）。", L"Could not create the shortcut (error code: {0})."}, // ShortcutFailed
    {L"打开日志", L"Open log"}, // OpenLog
    {L"以管理员身份重启（显示帧数需要）", L"Restart as administrator (for FPS)"}, // RestartAdmin
    {L"退出", L"Exit"}, // Exit
    {L"恢复默认", L"Reset to defaults"}, // ResetDefaults
    {L"硬件四格", L"CPU / GPU / Memory / Processor"}, // HardwareItems
    {L"电池与充电", L"Battery"}, // BatteryItems
    {L"功耗走势图", L"Power chart"}, // PowerChartItems
    {L"进程前三", L"Top processes"}, // ProcessesItems
    {L"磁盘读写速度", L"Disk speed"}, // DiskIOItems
    {L"系统盘占用", L"System drive usage"}, // SystemDiskItems
    {L"电量 %", L"Battery %"}, // BatteryPercent
    {L"续航/充满", L"Time left / Full in"}, // EtaItems
    {L"总功耗", L"Power"}, // TotalPower
    {L"CPU %", L"CPU %"}, // CpuPercent
    {L"内存 %", L"Memory %"}, // MemoryPercent
    {L"GPU %", L"GPU %"}, // GpuPercent
    {L"网速", L"Network speed"}, // NetworkSpeed
    {L"磁盘读写", L"Disk"}, // DiskIO
    {L"CPU", L"CPU"}, // Cpu
    {L"GPU", L"GPU"}, // Gpu
    {L"内存", L"Memory"}, // Memory
    {L"处理器", L"Processor"}, // Processor
    {L"处理器", L"CPU pkg"}, // CompactProcessor
    {L"整颗芯片", L"Whole chip"}, // WholeChip
    {L"整机", L"System"}, // System
    {L"电池", L"Battery"}, // Battery
    {L"网络", L"Network"}, // Network
    {L"延迟", L"Latency"}, // Latency
    {L"占用最高的进程", L"Top processes"}, // TopProcesses
    {L"系统盘", L"System drive"}, // SystemDrive
    {L"系统信息", L"System info"}, // SystemInfo
    {L"显卡", L"GPU"}, // GpuMemory (settings key remains gpu_memory)
    {L"Wi-Fi", L"Wi-Fi"}, // Wifi
    {L"电池健康", L"Battery health"}, // BatteryHealth
    {L"帧数", L"FPS"}, // Fps
    {L"FPS", L"FPS"}, // FpsShort
    {L"CPU 频率", L"CPU frequency"}, // CpuFrequency
    {L"频率", L"Clock"}, // Frequency
    {L"内存 已用/总量", L"Memory used / total"}, // MemoryUsedTotal
    {L"进程数", L"Processes"}, // ProcessCount
    {L"开机时长", L"Uptime"}, // Uptime
    {L"核显共享", L"Shared"}, // SharedMemory
    {L"独显专用", L"Dedicated"}, // DedicatedMemory
    {L"信号强度", L"Signal strength"}, // SignalStrength
    {L"循环次数", L"Cycles"}, // CycleCount
    {L"读", L"Read"}, // Read
    {L"写", L"Write"}, // Write
    {L"到 20%", L"To 20%"}, // To20
    {L"功耗走势 · ", L"Power · "}, // PowerChartPrefix
    {L"剩余 ", L"Free "}, // FreePrefix
    {L"断网", L"Offline"}, // Offline
    {L"检测中", L"Checking"}, // Checking
    {L"较慢", L"Slow"}, // Slow
    {L"已充满", L"Fully charged"}, // FullyCharged
    {L"未充电", L"Not charging"}, // NotCharging
    {L"充满", L"Full in"}, // FullIn
    {L"续航", L"Time left"}, // TimeLeft
    {L"电量低", L"Low battery"}, // LowBattery
    {L"放电中", L"Discharging"}, // Discharging
    {L"充电中", L"Charging"}, // Charging
    {L"外接电源", L"On AC power"}, // OnAc
    {L"无电池", L"No battery"}, // NoBattery
    {L"读取中…", L"Reading…"}, // Reading
    {L"节电模式已开启", L"Battery saver on"}, // BatterySaver
    {L"电源模式 · ", L"Power mode · "}, // PowerModePrefix
    {L"预计可用", L"Time left"}, // EtaLeft
    {L"预计充满", L"Full in"}, // EtaFull
    {L"电池状态", L"Battery status"}, // BatteryStatus
    {L"（系统估计）", L" (system estimate)"}, // SystemEstimate
    {L"计算中…", L"Calculating…"}, // Calculating
    {L"电源供电", L"On AC power"}, // AcPower
    {L"未检测到电池", L"No battery detected"}, // NoBatteryDetected
    {L"充电功率", L"Power"}, // ChargePower
    {L"实时功耗", L"Power"}, // LivePower
    {L"功耗", L"Power"}, // Power
    {L"电池功率", L"Power"}, // BatteryPower
    {L"每小时充入", L"Per hour"}, // ChargePerHour
    {L"每小时耗电", L"Per hour"}, // DrainPerHour
    {L"每小时", L"Per hour"}, // PerHour
    {L"组件启动后已用 ", L"Since launch "}, // SinceLaunch
    {L"拔电后已用 ", L"Unplugged for "}, // SinceUnplugged
    {L" · 平均 ", L" · Average "}, // AveragePrefix
    {L"剩余 ", L"Remaining "}, // RemainingPrefix
    {L" Wh / 满电 ", L" Wh / Full "}, // FullCapacityPrefix
    {L"本次 ↓ ", L"Session ↓ "}, // SessionTraffic
    {L"网络较慢 · 连接可用", L"Slow · Connected"}, // SlowConnected
    {L"网络正常", L"Connected"}, // NetworkOk
    {L"正在检测连接…", L"Checking connection…"}, // CheckingConnection
    {L"本组件 ", L"Pulse "}, // SelfCpu
    {L"统计中…", L"Collecting…"}, // Counting
    {L"需管理员", L"Run as admin"}, // RunAsAdmin
    {L"需要管理员权限", L"Run as admin"}, // AdminRequired
    {L"网络连接断开", L"Network disconnected"}, // NetworkDisconnected
    {L"网络已恢复", L"Network restored"}, // NetworkRestored
    {L"已断开 ", L"Down for "}, // DownFor
    {L"{0} · {1} · 本次断网 {2}", L"{0} · {1} · Down for {2}"}, // OutageEvent
    {L"{0} 起 · {1}", L"{0} · {1}"}, // DisconnectedEvent
    {L"连接超时", L"Connection timed out"}, // Timeout
    {L"延迟过高 {0} ms", L"Latency too high: {0} ms"}, // HighLatency
    {L"网络连接已断开", L"Network disconnected"}, // LinkDisconnected
    {L"网络无响应", L"No response"}, // NoResponse
    {L"无法访问网络", L"Network unavailable"}, // UnknownReason
    {L"平衡", L"Balanced"}, // Balanced
    {L"最佳性能", L"Best performance"}, // BestPerformance
    {L"最佳能效", L"Best efficiency"}, // BestEfficiency
    {L"更长续航", L"Longer battery life"}, // LongerBattery
    {L"小时", L"h"}, // Hours
    {L"时", L"h"}, // HourShort
    {L"分钟", L"min"}, // Minutes
    {L"分", L"min"}, // MinuteShort
    {L"秒", L"s"}, // Seconds
    {L"天", L"d"}, // Days
    {L"W", L"W"}, // Watt
    {L"ms", L"ms"}, // Ms
    {L"GHz", L"GHz"}, // Ghz
    {L"GB", L"GB"}, // Gb
    {L"Wh", L"Wh"}, // Wh
    {L"fps", L"fps"}, // FpsUnit
    {L"B", L"B"}, // Byte
    {L"KB", L"KB"}, // Kb
    {L"MB", L"MB"}, // Mb
    {L"TB", L"TB"}, // Tb
    {L"KB/s", L"KB/s"}, // Kbps
    {L"MB/s", L"MB/s"}, // Mbps
    {L"GB/s", L"GB/s"}, // Gbps
    {L" 小时 ", L" h "}, // HourJoin
    {L" 时 ", L" h "}, // HourShortJoin
    {L" 天 ", L" d "}, // DayJoin
    {L" 分 ", L" min "}, // MinuteJoin
    {L" 分", L" min"}, // MinuteSuffix
    {L" 时", L" h"}, // HourSuffix
    {L" 秒", L" s"}, // SecondSuffix
    {L" W", L" W"}, // WattSuffix
    {L" ms", L" ms"}, // MsSuffix
    {L" GHz", L" GHz"}, // GhzSuffix
    {L" GB", L" GB"}, // GbSuffix
    {L" Wh", L" Wh"}, // WhSuffix
    {L" fps", L" fps"}, // FpsSuffix
    {L"读 ", L"Read "}, // ReadPrefix
    {L"写 ", L"Write "}, // WritePrefix
    {L"0.5 秒", L"0.5 s"}, // RefreshHalf
    {L"1 秒（默认）", L"1 s (default)"}, // RefreshOne
    {L"2 秒", L"2 s"}, // RefreshTwo
    {L"-- ms", L"-- ms"}, // UnknownLatency
    {L"K", L"K"}, // CompactK
    {L"M", L"M"}, // CompactM
    {L"G", L"G"}, // CompactG
    {L"任务栏模式", L"Taskbar mode"},
    {L"CPU", L"CPU"}, {L"内存", L"Mem"}, {L"功率", L"Pwr"}, {L"延迟", L"Ping"},
    {L"电量", L"Bat"}, {L"续航", L"ETA"}, {L"GPU", L"GPU"}, {L"频率", L"Freq"},
    {L"帧数", L"FPS"}, {L"读", L"Rd"}, {L"写", L"Wr"},
    {L"任务栏位置", L"Taskbar position"},
    {L"自动", L"Automatic"},
    {L"右侧", L"Right"},
    {L"左侧", L"Left"},
    {L"充满", L"Full"}, // TaskbarFull
    {L"GPU 温度", L"GPU temp"}, // GpuTemp
    {L"温度", L"Temp"}, // Temperature
    {L"GPU°", L"GPU°"}, // TaskbarGpuTemp
    {L"电源计划 · ", L"Power plan · "}, // PowerPlanPrefix
    {L"未知", L"Unknown"}, // PowerPlanUnknown
    {L"插电放电", L"AC + battery"}, // AssistChip
    {L"外接电源 · 电池放电", L"External power · Battery discharging"}, // ExternalDischarge
    {L"净放电", L"Net discharge"}, // NetDischarge
    {L"节电", L"Saver"}, // BatterySaverShort
    {L"采样中", L"Sampling"}, // MeterWarmup
    {L"无计数器", L"No counter"}, // MeterNoCounter
    {L"无此域", L"No domain"}, // MeterNoDomain
    {L"读取失败", L"Read failed"}, // MeterReadFailed
    {L"功耗诊断", L"Power diagnostics"}, // PowerDiagnostics
    {L"无法写入功耗诊断：{0}", L"Could not write the power diagnostic: {0}"}, // PowerDiagnosticsFailed
    {L"诊断已写入，但无法打开文件：{0}", L"The diagnostic was written, but the file could not be opened: {0}"}, // PowerDiagnosticsOpenFailed
    {L"无法启动功耗诊断（错误码：{0}）。", L"Could not start power diagnostics (error {0})."}, // PowerDiagnosticsLaunchFailed
};
static_assert(sizeof(strings) / sizeof(strings[0]) == static_cast<std::size_t>(Key::Count), "i18n table size");
Language current = Language::English;
}
namespace detail {
std::array<const std::wstring*, static_cast<std::size_t>(Key::Count)> active = [] {
    std::array<const std::wstring*, static_cast<std::size_t>(Key::Count)> result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = &strings[i].en;
    return result;
}();
}
Preference ParsePreference(const wchar_t* value) {
    if (value && std::wcscmp(value, L"zh") == 0) return Preference::Chinese;
    if (value && std::wcscmp(value, L"en") == 0) return Preference::English;
    return Preference::Auto;
}
const wchar_t* PreferenceValue(Preference value) {
    switch (value) {
    case Preference::Chinese: return L"zh";
    case Preference::English: return L"en";
    default: return L"auto";
    }
}
bool SetLanguage(Preference preference) {
    const Language language = preference == Preference::Chinese ? Language::Chinese :
        preference == Preference::English ? Language::English :
        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::Chinese : Language::English;
    if (current == language) return false;
    current = language;
    for (std::size_t i = 0; i < detail::active.size(); ++i)
        detail::active[i] = language == Language::Chinese ? &strings[i].zh : &strings[i].en;
    return true;
}
Language CurrentLanguage() { return current; }
const wchar_t* LocaleName() { return current == Language::Chinese ? L"zh-CN" : L"en-US"; }
std::wstring Format(Key key, std::initializer_list<std::wstring> arguments) {
    const auto& pattern = T(key);
    std::wstring result;
    result.reserve(pattern.size() + 64);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 2] == L'}' &&
            pattern[i + 1] >= L'0' && pattern[i + 1] <= L'9') {
            const auto index = static_cast<std::size_t>(pattern[i + 1] - L'0');
            if (index < arguments.size()) { result += *(arguments.begin() + index); i += 2; continue; }
        }
        result += pattern[i];
    }
    return result;
}
} // namespace pulse::i18n
