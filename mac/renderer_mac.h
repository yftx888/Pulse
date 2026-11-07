#pragma once
#import <AppKit/AppKit.h>
#include "settings_mac.h"
#include <array>
#include <string>
#include <vector>

namespace pulse::mac {
struct RectF { CGFloat left, top, right, bottom; };
inline RectF Rect(CGFloat x, CGFloat y, CGFloat width, CGFloat height) { return {x, y, x + width, y + height}; }
struct FullLayout {
    std::array<RectF, ui::FullComponents.size()> cards{};
    CGFloat height = 56;
    RectF Card(ui::FullComponent c) const { return cards[static_cast<std::size_t>(c)]; }
};
FullLayout MakeFullLayout(const ui::Components& components, CGFloat width);
NSColor* Color(unsigned rgb, CGFloat alpha = 1);
CGFloat TextWidth(const std::wstring& text, NSFont* font);
void DrawText(CGContextRef context, const std::wstring& text, RectF rect, NSFont* font,
              NSColor* color, bool right = false, bool wrap = false);
struct StatusRow { ui::CompactComponent component; std::wstring label, value; };
std::array<StatusRow, 2> StatusRows(const ui::TaskbarColumn& column, const ui::View& view);
CGFloat MinimumStatusWidth(const ui::TaskbarColumn& column, NSFont* font);
NSColor* StatusInk(ui::CompactComponent component, const ui::View& view, bool light, std::size_t row);
class Renderer {
public:
    void Configure(const ui::Components& components);
    CGFloat Width() const { return fullWidth_; }
    CGFloat Height() const { return MakeFullLayout(components_, fullWidth_).height; }
    void Full(CGContextRef context, const ui::View& view);
    void Toast(CGContextRef context, NSSize size, const NetEvent& event, const std::wstring& message);
    CGFloat ToastHeight(const std::wstring& message, CGFloat width) const;
private:
    CGContextRef context_ = nullptr; // Borrowed for one synchronous paint.
    ui::Components components_;
    bool configured_ = false;
    i18n::Language language_ = i18n::Language::English;
    CGFloat fullWidth_ = 320, networkLabelWidth_ = 0, latencyLabelWidth_ = 0;
    CGFloat readLabelWidth_ = 0, writeLabelWidth_ = 0, systemLabelWidth_ = 84;
    CGFloat Measure(const std::wstring& text, CGFloat size, bool bold = false, bool numeric = false) const;
    void Text(const std::wstring& text, RectF r, CGFloat size, NSColor* color, bool bold = false,
              bool numeric = false, bool right = false, bool wrap = false);
    void Line(CGFloat x1, CGFloat y1, CGFloat x2, CGFloat y2, NSColor* color, CGFloat width = 1);
    void Rounded(RectF r, CGFloat radius, NSColor* color);
    void Gradient(RectF r, NSColor* from, NSColor* to, bool vertical, CGFloat radius = 0);
    void Background(CGFloat width, CGFloat height);
    void Panel(RectF r);
    void Bar(RectF r, CGFloat fraction, NSColor* from, NSColor* to);
    void Ring(const ui::View& view, CGFloat x, CGFloat y);
    void Bolt(CGFloat x, CGFloat y, CGFloat size, NSColor* color);
    void ChartGrid(RectF r);
    void Chart(RectF r, const float* data, std::size_t count, NSColor* color, CGFloat maximum);
    void ValueUnit(const std::wstring& value, const std::wstring& unit, CGFloat x, CGFloat y, CGFloat size, NSColor* color);
    void UpdateFullLayout();
};
} // namespace pulse::mac
