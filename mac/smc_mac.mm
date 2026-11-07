#include "smc_mac.h"
#include "smc_reader.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace pulse { namespace mac {
namespace {
using Clock = std::chrono::steady_clock;
std::uint32_t BigEndian(const Reading& value, std::uint32_t size) {
    std::uint32_t result = 0;
    for (std::uint32_t i = 0; i < size; ++i) result = (result << 8) | value.bytes[i];
    return result;
}
std::int64_t SignedBigEndian(const Reading& value, std::uint32_t size) {
    const std::uint32_t u = BigEndian(value, size);
    return (value.bytes[0] & 0x80) ? static_cast<std::int64_t>(u) - (std::int64_t{1} << (size * 8)) : u;
}
struct Decoded { bool valid = false; double value = 0; };
Decoded Decode(const Reading& value, const SmcKeyInfo& info) {
    if (!value.call.ok) return {};
    const auto type = info.dataType, size = info.dataSize;
    double number = 0;
    if (type == Code("flt ") && size == 4) {
        // Apple Silicon is little-endian. memcpy avoids alignment/aliasing issues.
        float f = 0; std::memcpy(&f, value.bytes.data(), sizeof(f)); number = f;
    } else if ((type == Code("ui8 ") && size == 1) || (type == Code("ui16") && size == 2) ||
               (type == Code("ui32") && size == 4)) number = BigEndian(value, size);
    else if ((type == Code("si8 ") && size == 1) || (type == Code("si16") && size == 2) ||
             (type == Code("si32") && size == 4)) number = static_cast<double>(SignedBigEndian(value, size));
    else if (type == Code("sp78") && size == 2) number = static_cast<double>(SignedBigEndian(value, size)) / 256.0;
    else if (type == Code("fpe2") && size == 2) number = BigEndian(value, size) / 4.0;
    else return {}; // flag/ioft/unknown types deliberately stay raw.
    return {std::isfinite(number), number};
}
std::wstring ValueText(const Decoded& decoded) {
    if (!decoded.valid) return L"--";
    std::wostringstream out; out << std::fixed << std::setprecision(6) << decoded.value; return out.str();
}
void PrintRaw(std::wostream& out, const Reading& value, std::uint32_t size) {
    if (!value.call.ok) { out << L"--"; return; }
    out << L"[" << std::hex << std::setfill(L'0');
    for (std::uint32_t i = 0; i < size; ++i) {
        if (i) out << L" ";
        out << std::setw(2) << static_cast<unsigned>(value.bytes[i]);
    }
    out << std::dec << std::setfill(L' ') << L"]";
}
void PrintReading(std::wostream& out, std::uint32_t key, const CachedInfo& info, const Reading& value) {
    out << FourText(key) << L" type='" << FourText(info.info.dataType) << L"' length=" << info.info.dataSize
        << L" raw="; PrintRaw(out, value, info.info.dataSize);
    out << L" value=" << ValueText(Decode(value, info.info));
    if (!value.call.ok) { out << L" read_failed "; PrintCall(out, value.call); }
    else if (!Decode(value, info.info).valid) out << L"（未解码/非有限数/类型长度不符）";
    out << L"\n";
}
bool Listed(std::uint32_t key) {
    return (key >> 24) == static_cast<std::uint32_t>('P') ||
        key == Code("B0AC") || key == Code("B0AV") || key == Code("B0AP");
}
bool ExtraCandidate(std::uint32_t key) {
    const auto prefix = key & 0xffffff00u;
    return prefix == (Code("PST ") & 0xffffff00u) || prefix == (Code("PDT ") & 0xffffff00u) ||
           prefix == (Code("PPB ") & 0xffffff00u);
}
struct Observation {
    bool previous_ok = false, have_change = false;
    Reading previous;
    Decoded previous_value;
    unsigned changes = 0, valid_pairs = 0;
    double last_change = 0;
    std::vector<double> intervals;
    void Add(const Reading& current, const SmcKeyInfo& info, double at) {
        const auto decoded = Decode(current, info);
        if (!current.call.ok) { previous_ok = have_change = false; return; }
        if (previous_ok) {
            ++valid_pairs;
            const bool changed = decoded.valid && previous_value.valid ? decoded.value != previous_value.value :
                !std::equal(current.bytes.begin(), current.bytes.begin() + info.dataSize, previous.bytes.begin());
            if (changed) {
                ++changes;
                if (have_change) intervals.push_back(at - last_change);
                have_change = true; last_change = at;
            }
        }
        previous_ok = true; previous = current; previous_value = decoded;
    }
};
} // anonymous namespace

namespace detail {
void RunSmcDiagnosticsWithBattery(int samples, void (*output)(const std::wstring&),
                                  SmcBatteryReader read_battery, void* battery) {
    if (!output) return;
    samples = std::clamp(samples, 5, 600);
    const auto start = Clock::now();
    SmcReader reader;
    std::wostringstream opening;
    opening << L"SMC 只读诊断：selector=2，命令仅 5/8/9；仅 stdout，不写文件、不启动探测。\n";
    const bool opened = reader.Open(opening);
    output(opening.str());
    if (!opened) { output(L"#KEY=--；未枚举或采样。\n"); return; }
    const auto& count_info = reader.Info(Code("#KEY"));
    const auto count_value = reader.Read(Code("#KEY"), count_info);
    std::wostringstream count_text;
    PrintReading(count_text, Code("#KEY"), count_info, count_value);
    const bool count_ok = count_value.call.ok && count_info.info.dataType == Code("ui32") && count_info.info.dataSize == 4;
    const std::uint32_t count = count_ok ? BigEndian(count_value, 4) : 0;
    count_text << L"键总数=";
    if (count_ok) count_text << count; else count_text << L"--（#KEY 必须为 ui32/4 字节）";
    count_text << L"\n"; output(count_text.str());

    std::vector<std::uint32_t> candidates = {Code("PSTR"), Code("PDTR"), Code("PPBR"), Code("PHPC"), Code("PDBR"), Code("PMVC")};
    std::set<std::uint32_t> seen(candidates.begin(), candidates.end()), listed;
    const auto enumeration_start = Clock::now();
    std::uint32_t enum_ok = 0, enum_failed = 0;
    // Refuse a corrupt count rather than issue billions of driver calls.
    if (count_ok && count <= 65536u) {
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key = 0;
            const auto call = reader.KeyAt(i, key);
            if (!call.ok) {
                ++enum_failed;
                std::wostringstream out; out << L"index=" << i << L" read_failed "; PrintCall(out, call);
                out << L"\n"; output(out.str()); continue;
            }
            ++enum_ok;
            if (ExtraCandidate(key) && seen.insert(key).second) candidates.push_back(key);
            if (!Listed(key) || !listed.insert(key).second) continue;
            const auto& info = reader.Info(key);
            std::wostringstream out; PrintReading(out, key, info, reader.Read(key, info)); output(out.str());
        }
    } else output(L"枚举跳过：键总数不可用或超过安全上限 65536；仍检查固定候选键。\n");
    std::wostringstream enumeration;
    enumeration << std::fixed << std::setprecision(3) << L"enumeration_ms="
        << std::chrono::duration<double, std::milli>(Clock::now() - enumeration_start).count()
        << L" indexed_ok=" << enum_ok << L" indexed_failed=" << enum_failed << L" listed=" << listed.size()
        << L"（包含元数据/筛选键字节读取及打印）\n";
    output(enumeration.str());

    std::vector<std::uint32_t> active;
    for (const auto key : candidates) {
        const auto& info = reader.Info(key);
        std::wostringstream out; out << L"candidate=" << FourText(key) << L" ";
        if (info.call.ok && info.info.dataSize && info.info.dataSize <= 32) {
            active.push_back(key);
            out << L"available type='" << FourText(info.info.dataType) << L"' length=" << info.info.dataSize;
        } else {
            out << L"unavailable length=" << info.info.dataSize;
            if (info.call.ok) out << L"（长度须为 1–32；未发字节读取命令）";
            else { out << L" info_failed "; PrintCall(out, info.call); }
        }
        out << L"\n"; output(out.str());
    }
    output(L"采样：t_s 为诊断启动后的秒数；每键 read_ms 仅含缓存元数据下的一次字节读取。\n"
           L"候选数值按 W 展示，键语义/单位待真机确认；未知类型只保留 raw；0 和负数原样保留。\n"
           L"变化比较使用未舍入解码数值；无法解码时比较 raw。失败行打断相邻比较和变化间隔。\n");
    std::vector<Observation> observations(active.size());
    auto due = Clock::now();
    for (int i = 0; i < samples; ++i) {
        std::this_thread::sleep_until(due);
        std::wostringstream out; out << std::fixed << std::setprecision(3)
            << L"t_s=" << Seconds(Clock::now(), start);
        for (std::size_t k = 0; k < active.size(); ++k) {
            const auto key = active[k];
            const auto& info = reader.Info(key); // Already cached, including failures; no metadata driver call.
            const auto before = Clock::now();
            const auto value = reader.Read(key, info);
            const auto after = Clock::now();
            const auto decoded = Decode(value, info.info);
            observations[k].Add(value, info.info, Seconds(after, start));
            out << L" " << FourText(key) << L"_W=" << ValueText(decoded) << L" raw=";
            PrintRaw(out, value, info.info.dataSize);
            out << L" read_ms=" << std::chrono::duration<double, std::milli>(after - before).count();
            if (!value.call.ok) { out << L" read_failed "; PrintCall(out, value.call); }
        }
        const auto& b0info = reader.Info(Code("B0AP"));
        const auto b0 = reader.Read(Code("B0AP"), b0info);
        out << L" B0AP_raw="; PrintRaw(out, b0, b0info.info.dataSize);
        out << L" B0AP_le_si32_mW=";
        if (b0.call.ok && b0info.info.dataSize == 4 && b0info.info.dataType == Code("si32")) {
            std::int32_t signed_mw = 0;
            std::memcpy(&signed_mw, b0.bytes.data(), sizeof(signed_mw));
            out << signed_mw;
        } else out << L"--";
        // Sequential same-row reads, not an atomic cross-device snapshot.
        if (read_battery) out << read_battery(battery);
        out << L"\n"; output(out.str());
        due += std::chrono::seconds(1);
        if (due < Clock::now()) due = Clock::now() + std::chrono::seconds(1);
    }
    std::wostringstream summary; summary << std::fixed << std::setprecision(3)
        << L"SMC 相邻有效行变化统计（1 秒观察分辨率；无变化不代表传感器未刷新）：\n";
    for (std::size_t k = 0; k < active.size(); ++k) {
        auto& obs = observations[k];
        summary << FourText(active[k]) << L" changes=" << obs.changes << L" valid_pairs=" << obs.valid_pairs;
        if (obs.changes < 2 || obs.intervals.empty()) summary << L" 观察不足";
        else {
            std::sort(obs.intervals.begin(), obs.intervals.end());
            const auto n = obs.intervals.size();
            const double median = n % 2 ? obs.intervals[n / 2] : (obs.intervals[n / 2 - 1] + obs.intervals[n / 2]) / 2;
            summary << L" change_interval_s median=" << median << L" min=" << obs.intervals.front()
                << L" max=" << obs.intervals.back() << L" intervals=" << n;
        }
        summary << L"\n";
    }
    output(summary.str());
}
} // namespace detail
} } // namespace pulse::mac
