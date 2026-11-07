#pragma once
#import <Foundation/Foundation.h>
#include "../src/ui_prefs.h"
#include <array>
#include <string>

namespace pulse::mac {
inline constexpr std::array<int, 7> SlowThresholds{{50, 100, 150, 200, 300, 500, 800}};
bool Supported(ui::CompactComponent component);
bool Supported(ui::FullComponent component);
void Sanitize(ui::Components& components);
struct AppSettings {
    ui::Components components;
    ui::TextColors colors{};
    unsigned refresh_ms = 2000;
    int slow_ms = 500;
    bool notify = true, sound = true;
    i18n::Preference language = i18n::Preference::Auto;
    std::wstring directory, file;
    bool Load(std::wstring& error);
    bool Save(std::wstring& error) const;
private:
    // An unreadable existing ini must never be overwritten during this run.
    mutable bool write_blocked_ = false;
};
} // namespace pulse::mac
