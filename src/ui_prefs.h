#pragma once
#include "ui_model.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {
inline constexpr std::array<unsigned, 3> MiniRefreshIntervals{{1000u, 2000u, 5000u}};
inline constexpr std::array<int, 5> MiniOpacities{{100, 90, 80, 70, 60}};
inline unsigned ParseMiniRefresh(std::wstring_view value) {
    return value == L"1000" ? 1000u : value == L"5000" ? 5000u : 2000u;
}
inline int ParseMiniOpacity(std::wstring_view value) {
    for (int percent : MiniOpacities) if (value == std::to_wstring(percent)) return percent;
    return 100;
}
inline unsigned MiniTimerTolerance(unsigned interval) {
    return interval == 1000u ? 150u : interval == 5000u ? 500u : 300u;
}
inline ComponentOrder ParseComponentOrder(std::wstring_view value, const ComponentOrder& defaults) {
    ComponentOrder result{};
    std::array<bool, CompactComponents.size()> seen{};
    std::size_t count = 0;
    while (!value.empty()) {
        const auto comma = value.find(L',');
        const auto key = value.substr(0, comma);
        for (std::size_t i = 0; i < CompactComponents.size(); ++i) {
            if (!seen[i] && key == CompactComponents[i].key) {
                result[count++] = static_cast<CompactComponent>(i); seen[i] = true; break;
            }
        }
        if (comma == std::wstring_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    for (auto c : defaults) {
        const auto i = static_cast<std::size_t>(c);
        if (!seen[i]) { result[count++] = c; seen[i] = true; }
    }
    return result;
}
inline std::wstring FormatComponentOrder(const ComponentOrder& order) {
    std::wstring result;
    for (auto c : order) {
        if (!result.empty()) result += L',';
        result += CompactComponents[static_cast<std::size_t>(c)].key;
    }
    return result;
}
using ComponentSelection = std::array<bool, CompactComponents.size()>;
template<std::size_t N> inline void SelectComponents(std::array<bool, N>& selection, bool enabled) {
    selection.fill(enabled);
}
enum class ComponentMove { Up, Down, First, Last };
inline bool CanMoveComponent(const ComponentOrder& order, const ComponentSelection& enabled,
                             CompactComponent component, bool up) {
    const auto at = std::find(order.begin(), order.end(), component);
    if (at == order.end() || !enabled[static_cast<std::size_t>(component)]) return false;
    if (up) {
        for (auto it = order.begin(); it != at; ++it)
            if (enabled[static_cast<std::size_t>(*it)]) return true;
    } else {
        for (auto it = at + 1; it != order.end(); ++it)
            if (enabled[static_cast<std::size_t>(*it)]) return true;
    }
    return false;
}
inline bool MoveComponent(ComponentOrder& order, const ComponentSelection& enabled,
                          CompactComponent component, ComponentMove move) {
    // Rotate values only in enabled slots: disabled entries keep their positions and order.
    std::array<std::size_t, CompactComponents.size()> slots{};
    std::size_t count = 0, current = order.size();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (!enabled[static_cast<std::size_t>(order[i])]) continue;
        if (order[i] == component) current = count;
        slots[count++] = i;
    }
    if (current >= count) return false;
    std::size_t target = current;
    switch (move) {
    case ComponentMove::Up: if (current) --target; break;
    case ComponentMove::Down: if (current + 1 < count) ++target; break;
    case ComponentMove::First: target = 0; break;
    case ComponentMove::Last: target = count - 1; break;
    }
    if (current == target) return false;
    while (current != target) {
        const auto next = current < target ? current + 1 : current - 1;
        std::swap(order[slots[current]], order[slots[next]]); current = next;
    }
    return true;
}
struct TaskbarColumn {
    std::array<CompactComponent, 2> items{};
    std::size_t count = 0;
    bool speed = false; // Network or disk owns both rows of one column.
    bool operator==(const TaskbarColumn& r) const {
        return items == r.items && count == r.count && speed == r.speed;
    }
};
inline std::vector<TaskbarColumn> TaskbarColumns(const ComponentOrder& order, const ComponentSelection& enabled) {
    std::vector<TaskbarColumn> result;
    TaskbarColumn pending;
    const bool pair = enabled[static_cast<std::size_t>(CompactComponent::Eta)] &&
        enabled[static_cast<std::size_t>(CompactComponent::Power)];
    bool paired = false;
    auto flush = [&] { if (pending.count) { result.push_back(pending); pending = {}; } };
    for (auto c : order) {
        if (!enabled[static_cast<std::size_t>(c)]) continue;
        const bool speed = c == CompactComponent::Network || c == CompactComponent::DiskIO;
        if (pair && (c == CompactComponent::Eta || c == CompactComponent::Power)) {
            if (!paired) {
                flush();
                // ETA remains the upper row, even when Power determines the column position.
                result.push_back({{CompactComponent::Eta, CompactComponent::Power}, 2, false});
                paired = true;
            }
        } else if (speed) {
            flush(); result.push_back({{c, c}, 1, true});
        } else {
            pending.items[pending.count++] = c;
            if (pending.count == 2) flush();
        }
    }
    flush();
    return result;
}
using TextColors = std::array<std::optional<std::uint32_t>, CompactComponents.size()>;
struct MiniAppearance {
    int opacity = 100;
    TextColors compact{}, taskbar{};
};
inline std::optional<std::uint32_t> ParseTextColor(std::wstring_view value) {
    if (value.size() != 7 || value[0] != L'#') return std::nullopt;
    std::uint32_t rgb = 0;
    for (std::size_t i = 1; i < value.size(); ++i) {
        const wchar_t c = value[i];
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' :
            c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        rgb = (rgb << 4) | static_cast<std::uint32_t>(digit);
    }
    return rgb;
}
inline std::wstring FormatTextColor(std::optional<std::uint32_t> rgb) {
    if (!rgb || *rgb > 0xffffffu) return {};
    std::wstring value = L"#000000";
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (std::size_t i = 0; i < 6; ++i) value[6 - i] = hex[(*rgb >> (4 * i)) & 15u];
    return value;
}
inline bool PreserveValueColor(CompactComponent c, const View& v) {
    switch (c) {
    case CompactComponent::Battery: return (v.battery >= 0 && v.battery <= .20f) || v.supplemental;
    case CompactComponent::Eta: return v.supplemental;
    case CompactComponent::Power: return v.low || v.supplemental;
    case CompactComponent::Latency: return v.netState == NetState::Slow || v.netState == NetState::Offline;
    case CompactComponent::GpuTemp: return v.gpuTempLevel >= 1;
    case CompactComponent::Fps: return v.fpsAdmin;
    default: return false;
    }
}
} // namespace pulse::ui
