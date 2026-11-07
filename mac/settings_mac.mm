#include "settings_mac.h"
#include "mac_strings.h"
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pulse::mac {
bool Supported(ui::CompactComponent c) {
    return c != ui::CompactComponent::CpuFrequency && c != ui::CompactComponent::Fps &&
        c != ui::CompactComponent::GpuTemp && c != ui::CompactComponent::Count;
}
bool Supported(ui::FullComponent c) {
    return c != ui::FullComponent::GpuMemory && c != ui::FullComponent::Wifi &&
        c != ui::FullComponent::Fps && c != ui::FullComponent::Count;
}
void Sanitize(ui::Components& c) {
    for (std::size_t i = 0; i < c.taskbar.size(); ++i) if (!Supported(static_cast<ui::CompactComponent>(i))) c.taskbar[i] = false;
    for (std::size_t i = 0; i < c.full.size(); ++i) if (!Supported(static_cast<ui::FullComponent>(i))) c.full[i] = false;
}
namespace {
using Ini = std::map<std::wstring, std::map<std::wstring, std::wstring>>;
std::wstring Trim(std::wstring s) {
    const auto first = s.find_first_not_of(L" \t\r\n"), last = s.find_last_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L"" : s.substr(first, last - first + 1);
}
std::wstring Lower(std::wstring s) { for (auto& c : s) c = std::towlower(c); return s; }
Ini Parse(NSString* text) {
    Ini ini;
    std::wstring section;
    for (NSString* line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        auto s = Trim(ToWide(line));
        if (s.empty() || s[0] == L';' || s[0] == L'#') continue;
        if (s[0] == L'[' && s.back() == L']') section = Lower(Trim(s.substr(1, s.size() - 2)));
        else if (const auto at = s.find(L'='); at != std::wstring::npos && !section.empty())
            ini[section][Lower(Trim(s.substr(0, at)))] = Trim(s.substr(at + 1));
    }
    return ini;
}
std::wstring Get(const Ini& ini, const wchar_t* section, const std::wstring& key) {
    auto s = ini.find(Lower(section)); if (s == ini.end()) return {};
    auto v = s->second.find(Lower(key)); return v == s->second.end() ? L"" : v->second;
}
bool Bool(const std::wstring& s, bool fallback) { return s == L"0" ? false : s == L"1" ? true : fallback; }
NSString* ReadIniText(NSString* path, bool& exists) {
    exists = [NSFileManager.defaultManager fileExistsAtPath:path];
    NSData* data = [NSData dataWithContentsOfFile:path options:0 error:nil];
    if (!data) return nil;
    exists = true;
    NSString* text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    const auto* bytes = static_cast<const unsigned char*>(data.bytes);
    const bool utf16_bom = data.length >= 2 &&
        ((bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == 0xfe && bytes[1] == 0xff));
    if (!text && utf16_bom) text = [[NSString alloc] initWithData:data encoding:NSUTF16StringEncoding];
    if (!text) text = [[NSString alloc] initWithData:data
        encoding:CFStringConvertEncodingToNSStringEncoding(kCFStringEncodingGB_18030_2000)];
    if ([text hasPrefix:@"\ufeff"]) text = [text substringFromIndex:1];
    return text;
}
void WarnSettingsReadFailure() {
    std::fputs("Pulse: settings.ini could not be read or decoded; using defaults without writing this file.\n", stderr);
}
}
bool AppSettings::Load(std::wstring& error) {
    error.clear();
    directory = ToWide([NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Pulse"]);
    file = directory + L"/settings.ini";
    NSError* e = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:ToNSString(directory) withIntermediateDirectories:YES attributes:nil error:&e]) {
        error = ToWide(e.localizedDescription); return false;
    }
    bool exists = false;
    NSString* text = ReadIniText(ToNSString(file), exists);
    if (!text && exists) {
        if (!write_blocked_) WarnSettingsReadFailure();
        write_blocked_ = true;
    }
    const auto ini = Parse(text ?: @"");
    language = i18n::ParsePreference(Get(ini, L"Window", L"language").c_str());
    refresh_ms = ui::ParseMiniRefresh(Get(ini, L"Window", L"mini_refresh_ms"));
    slow_ms = 500;
    for (int n : SlowThresholds) if (Get(ini, L"Network", L"slow_ms") == std::to_wstring(n)) slow_ms = n;
    notify = Bool(Get(ini, L"Network", L"notify"), true);
    sound = Bool(Get(ini, L"Network", L"notify_sound"), true);
    components.taskbarOrder = ui::ParseComponentOrder(Get(ini, L"Taskbar", L"order"), ui::DefaultTaskbarOrder);
    for (std::size_t i = 0; i < components.taskbar.size(); ++i) {
        const auto& info = ui::TaskbarComponents[i];
        components.taskbar[i] = Bool(Get(ini, L"Taskbar", info.key), info.enabled);
        colors[i] = ui::ParseTextColor(Get(ini, L"Taskbar", std::wstring(L"color_") + info.key));
    }
    for (std::size_t i = 0; i < components.full.size(); ++i) {
        const auto& info = ui::FullComponents[i];
        components.full[i] = Bool(Get(ini, L"Full", info.key), info.enabled);
    }
    Sanitize(components);
    return true;
}
bool AppSettings::Save(std::wstring& error) const {
    error.clear();
    if (write_blocked_) return true;
    // Decode the original with the same fallback as Load before preserving its
    // unknown and unsupported Windows keys. Never replace an unreadable file.
    bool exists = false;
    NSString* old = ReadIniText(ToNSString(file), exists);
    if (!old && exists) {
        WarnSettingsReadFailure(); write_blocked_ = true; return true;
    }
    auto ini = Parse(old ?: @"");
    auto set = [&](const wchar_t* section, const std::wstring& key, const std::wstring& value) { ini[Lower(section)][Lower(key)] = value; };
    set(L"Window", L"language", i18n::PreferenceValue(language));
    set(L"Window", L"mini_refresh_ms", std::to_wstring(refresh_ms));
    set(L"Network", L"slow_ms", std::to_wstring(slow_ms));
    set(L"Network", L"notify", notify ? L"1" : L"0");
    set(L"Network", L"notify_sound", sound ? L"1" : L"0");
    set(L"Taskbar", L"order", ui::FormatComponentOrder(components.taskbarOrder));
    for (std::size_t i = 0; i < components.taskbar.size(); ++i) {
        if (!Supported(static_cast<ui::CompactComponent>(i))) continue;
        set(L"Taskbar", ui::TaskbarComponents[i].key, components.taskbar[i] ? L"1" : L"0");
        const auto key = std::wstring(L"color_") + ui::TaskbarComponents[i].key;
        if (colors[i]) set(L"Taskbar", key, ui::FormatTextColor(colors[i]));
        else ini[Lower(L"Taskbar")].erase(Lower(key));
    }
    for (std::size_t i = 0; i < components.full.size(); ++i) {
        if (!Supported(static_cast<ui::FullComponent>(i))) continue;
        set(L"Full", ui::FullComponents[i].key, components.full[i] ? L"1" : L"0");
    }
    std::wstring contents;
    for (const auto& section : ini) {
        const auto& name = section.first;
        const std::wstring canonical = name == L"window" ? L"Window" : name == L"taskbar" ? L"Taskbar" :
            name == L"full" ? L"Full" : name == L"network" ? L"Network" : name;
        contents += L"[" + canonical + L"]\n";
        for (const auto& v : section.second) contents += v.first + L"=" + v.second + L"\n";
        contents += L"\n";
    }
    auto bytes = Utf8(contents);
    auto name = Utf8(file + L".XXXXXX");
    std::vector<char> temporary(name.begin(), name.end()); temporary.push_back(0);
    const int fd = mkstemp(temporary.data());
    if (fd < 0) { error = FromUtf8(std::strerror(errno)); return false; }
    bool ok = true; int saved = 0;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto n = write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; saved = n < 0 ? errno : EIO; break; }
        written += static_cast<std::size_t>(n);
    }
    if (ok && fsync(fd) != 0) { ok = false; saved = errno; }
    if (close(fd) != 0 && ok) { ok = false; saved = errno; }
    if (ok && rename(temporary.data(), Utf8(file).c_str()) != 0) { ok = false; saved = errno; }
    if (!ok) { unlink(temporary.data()); error = FromUtf8(std::strerror(saved)); }
    return ok;
}
} // namespace pulse::mac
