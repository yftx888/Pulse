#include "renderer_mac.h"
#include "mac_strings.h"
#import <CoreText/CoreText.h>
#include <algorithm>
#include <cmath>

namespace pulse::mac {
using namespace ui;
NSColor* Color(unsigned rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0 green:((rgb >> 8) & 255) / 255.0
        blue:(rgb & 255) / 255.0 alpha:alpha];
}
namespace {
NSColor* Alpha(NSColor* color, CGFloat alpha) { return [color colorWithAlphaComponent:alpha]; }
NSColor* const TextColor = Color(0xe9edf6), * const Muted = Color(0x8b94aa), * const Faint = Color(0x586079);
NSColor* const Down = Color(0x38bdf8), * const Up = Color(0xa78bfa), * const Drain = Color(0xfbbf24);
NSColor* const Charge = Color(0x34d399), * const Ac = Color(0x60a5fa), * const Danger = Color(0xf87171);
NSColor* const Track = Color(0xffffff, .07), * const Border = Color(0xffffff, .07);
NSColor* Accent(const View& v) {
    if (v.low) return Danger;
    if (v.mode == PowerMode::Charging) return Charge;
    if (v.mode == PowerMode::AcIdle || v.mode == PowerMode::NoBattery) return Ac;
    return Drain;
}
NSColor* BatteryColor(const View& v) {
    return v.battery < 0 ? Faint : v.battery <= .20f ? Danger : v.battery <= .45f ? Drain : Charge;
}
NSColor* NetworkColor(NetState state) {
    return state == NetState::Online ? Charge : state == NetState::Slow ? Drain : state == NetState::Offline ? Danger : Faint;
}
NSFont* Font(CGFloat size, bool bold = false, bool numeric = false) {
    return numeric ? [NSFont monospacedDigitSystemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular] :
        [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
}
CGRect CRect(RectF r) { return CGRectMake(r.left, r.top, std::max<CGFloat>(0, r.right-r.left), std::max<CGFloat>(0, r.bottom-r.top)); }
CGPathRef Path(RectF r, CGFloat radius) { return CGPathCreateWithRoundedRect(CRect(r), radius, radius, nullptr); }
NSAttributedString* Attributed(const std::wstring& text, NSFont* font, NSColor* color) {
    return [[NSAttributedString alloc] initWithString:ToNSString(text) attributes:@{NSFontAttributeName:font, NSForegroundColorAttributeName:color}];
}
Key Label(CompactComponent c, PowerMode mode) {
    switch (c) {
    case CompactComponent::Battery: return Key::TaskbarBattery;
    case CompactComponent::Eta: return mode == PowerMode::Charging ? Key::TaskbarFull : Key::TaskbarEta;
    case CompactComponent::Power: return Key::TaskbarPower;
    case CompactComponent::Cpu: return Key::TaskbarCpu;
    case CompactComponent::Memory: return Key::TaskbarMemory;
    case CompactComponent::Gpu: return Key::TaskbarGpu;
    case CompactComponent::Latency: return Key::TaskbarPing;
    default: return Key::TaskbarRead;
    }
}
}
CGFloat TextWidth(const std::wstring& text, NSFont* font) {
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)Attributed(text, font, NSColor.labelColor));
    const CGFloat width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line); return std::ceil(width);
}
void DrawText(CGContextRef context, const std::wstring& text, RectF rect, NSFont* font, NSColor* color, bool right, bool wrap) {
    if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return;
    CGContextSaveGState(context);
    CGContextClipToRect(context, CRect(rect));
    CGContextTranslateCTM(context, rect.left, rect.bottom);
    CGContextScaleCTM(context, 1, -1); CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    NSAttributedString* attributed = Attributed(text, font, color);
    if (wrap) {
        CTFramesetterRef framesetter = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
        CGPathRef path = CGPathCreateWithRect(CGRectMake(0, 0, rect.right-rect.left, rect.bottom-rect.top), nullptr);
        CTFrameRef frame = CTFramesetterCreateFrame(framesetter, CFRangeMake(0, 0), path, nullptr);
        CTFrameDraw(frame, context); CFRelease(frame); CGPathRelease(path); CFRelease(framesetter);
    } else {
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
        CGFloat ascent = 0, descent = 0;
        CGFloat width = CTLineGetTypographicBounds(line, &ascent, &descent, nullptr);
        if (width > rect.right-rect.left) {
            CTLineRef token = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)Attributed(L"…", font, color));
            CTLineRef truncated = CTLineCreateTruncatedLine(line, rect.right-rect.left, kCTLineTruncationEnd, token);
            CFRelease(token);
            if (truncated) { CFRelease(line); line = truncated; width = CTLineGetTypographicBounds(line, &ascent, &descent, nullptr); }
        }
        CGContextSetTextPosition(context, right ? std::max<CGFloat>(0, rect.right-rect.left-width) : 0,
            (rect.bottom-rect.top-ascent-descent)/2+descent);
        CTLineDraw(line, context); CFRelease(line);
    }
    CGContextRestoreGState(context);
}
FullLayout MakeFullLayout(const Components& components, CGFloat width) {
    FullLayout layout; CGFloat y = 44; bool any = false;
    for (auto c : FullOrder) {
        if (!components.Enabled(c) || !Supported(c)) continue;
        if (any) y += 8;
        const auto i = static_cast<std::size_t>(c);
        // Mac omits the unsupported CPU-frequency row in SystemInfo.
        const CGFloat h = c == FullComponent::SystemInfo ? 96 : FullComponents[i].extent;
        layout.cards[i] = Rect(12, y, width-24, h); y += h; any = true;
    }
    layout.height = y+12; return layout;
}
std::array<StatusRow, 2> StatusRows(const TaskbarColumn& column, const View& v) {
    std::array<StatusRow, 2> rows{};
    for (std::size_t row = 0; row < 2; ++row) {
        if (!column.speed && row >= column.count) continue;
        auto& r = rows[row]; r.component = column.items[column.speed ? 0 : row];
        const auto c = r.component;
        if (column.speed) {
            const bool network = c == CompactComponent::Network;
            r.label = network ? (row ? L"↓" : L"↑") : T(row ? Key::TaskbarWrite : Key::TaskbarRead);
            r.value = network ? (row ? v.compactDown : v.compactUp) : (row ? v.diskWrite : v.diskRead);
        } else {
            r.label = T(Label(c, v.mode));
            switch (c) {
            case CompactComponent::Battery: r.value = v.percent + L"%"; break;
            case CompactComponent::Eta:
                r.value = !v.compactEtaVisible || v.compactEtaValue.empty() ? L"--" : v.compactEtaValue;
                if (v.compactEtaVisible && v.compactEtaLabel.empty()) r.label.clear(); break;
            case CompactComponent::Power:
                if (!v.powerLabel.empty()) r.label = v.powerLabel;
                r.value = v.powerValue + T(Key::WattSuffix); break;
            case CompactComponent::Cpu: r.value = v.cpuValue; break;
            case CompactComponent::Memory: r.value = v.memoryValue; break;
            case CompactComponent::Gpu: r.value = v.gpuUsage; break;
            case CompactComponent::Latency: r.value = v.latency; break;
            default: break;
            }
        }
    }
    return rows;
}
CGFloat MinimumStatusWidth(const TaskbarColumn& column, NSFont* font) {
    CGFloat result = 0;
    for (std::size_t row = 0; row < (column.speed ? 2 : column.count); ++row) {
        const auto c = column.items[column.speed ? 0 : row];
        CGFloat label = TextWidth(T(Label(c, PowerMode::Unknown)), font), value = TextWidth(L"100%", font);
        auto fit = [&](const std::wstring& s) { value = std::max(value, TextWidth(s, font)); };
        if (column.speed) {
            label = TextWidth(c == CompactComponent::Network ? L"↓" : T(Key::TaskbarWrite), font);
            for (auto u : {Key::Kbps, Key::Mbps, Key::Gbps}) { fit(L"100.0 " + T(u)); fit(L"1000 " + T(u)); }
        } else if (c == CompactComponent::Eta) {
            label = std::max(label, TextWidth(T(Key::TaskbarFull), font));
            fit(L"88:88"); fit(L"59" + T(Key::MinuteSuffix)); fit(L"99+" + T(Key::HourSuffix)); fit(T(Key::AssistChip));
        } else if (c == CompactComponent::Power) {
            for (auto k : {Key::System, Key::CompactProcessor, Key::Battery, Key::Power}) label = std::max(label, TextWidth(T(k), font));
            fit(L"100.0" + T(Key::WattSuffix)); fit(L"9999" + T(Key::WattSuffix));
        } else if (c == CompactComponent::Latency) {
            fit(L"999" + T(Key::MsSuffix)); fit(T(Key::UnknownLatency)); fit(T(Key::Checking)); fit(T(Key::Offline));
        }
        // extent is for a single-row strip; two-row columns use the same content reservations.
        const CGFloat minimum = CompactComponents[static_cast<std::size_t>(c)].extent;
        result = std::max({result, label+4+value, minimum > 0 ? std::min<CGFloat>(minimum, 89) : 0});
    }
    return std::ceil(result);
}
NSColor* StatusInk(CompactComponent c, const View& v, bool light, std::size_t row) {
    const auto color = [light](unsigned dark, unsigned pale) { return Color(light ? pale : dark); };
    NSColor* normal = color(0xe9edf6, 0x202634), *muted = color(0xb0bbd0, 0x41495b);
    NSColor* up = color(0xa78bfa, 0x6942b3), *down = color(0x38bdf8, 0x006c9d);
    NSColor* green = color(0x34d399, 0x00774b), *yellow = color(0xfbbf24, 0x865b00), *red = color(0xf87171, 0xb51c27);
    switch (c) {
    case CompactComponent::Network: return row ? down : up;
    case CompactComponent::DiskIO: return row ? up : down;
    case CompactComponent::Battery:
        return v.supplemental ? yellow : v.mode == PowerMode::Charging || (v.mode == PowerMode::AcIdle && v.acOnline) ? green :
            v.battery >= 0 && v.battery <= .2f ? red : normal;
    case CompactComponent::Eta: return v.supplemental ? yellow : v.mode == PowerMode::Charging ? green : normal;
    case CompactComponent::Power:
        // Supplemental battery power has priority over the low-battery warning.
        return v.supplemental ? yellow : v.low ? red : v.mode == PowerMode::Charging ? green : yellow;
    case CompactComponent::Cpu: return down;
    case CompactComponent::Memory: return up;
    case CompactComponent::Gpu: return green;
    case CompactComponent::Latency:
        return v.netState == NetState::Offline ? red : v.netState == NetState::Slow ? yellow :
            v.netState == NetState::Checking ? muted : green;
    default: return normal;
    }
}
CGFloat Renderer::Measure(const std::wstring& text, CGFloat size, bool bold, bool numeric) const { return TextWidth(text, Font(size, bold, numeric)); }
void Renderer::Text(const std::wstring& text, RectF r, CGFloat size, NSColor* color, bool bold, bool numeric, bool right, bool wrap) {
    DrawText(context_, text, r, Font(size, bold, numeric), color, right, wrap);
}
void Renderer::Line(CGFloat x1, CGFloat y1, CGFloat x2, CGFloat y2, NSColor* color, CGFloat width) {
    CGContextSetStrokeColorWithColor(context_, color.CGColor); CGContextSetLineWidth(context_, width);
    CGContextBeginPath(context_); CGContextMoveToPoint(context_, x1, y1); CGContextAddLineToPoint(context_, x2, y2); CGContextStrokePath(context_);
}
void Renderer::Rounded(RectF r, CGFloat radius, NSColor* color) {
    CGPathRef path = Path(r, radius); CGContextAddPath(context_, path);
    CGContextSetFillColorWithColor(context_, color.CGColor); CGContextFillPath(context_); CGPathRelease(path);
}
void Renderer::Gradient(RectF r, NSColor* from, NSColor* to, bool vertical, CGFloat radius) {
    CGContextSaveGState(context_);
    CGPathRef path = Path(r, radius); CGContextAddPath(context_, path); CGContextClip(context_); CGPathRelease(path);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    NSArray* colors = @[(__bridge id)from.CGColor, (__bridge id)to.CGColor];
    CGGradientRef gradient = CGGradientCreateWithColors(space, (__bridge CFArrayRef)colors, nullptr);
    if (gradient) {
        CGContextDrawLinearGradient(context_, gradient, CGPointMake(r.left,r.top), vertical ? CGPointMake(r.left,r.bottom) : CGPointMake(r.right,r.top),
            kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation); CGGradientRelease(gradient);
    }
    CGColorSpaceRelease(space); CGContextRestoreGState(context_);
}
void Renderer::Background(CGFloat width, CGFloat height) {
    Gradient(Rect(0,0,width,height), Color(0x151c2f), Color(0x0c111d), true);
    auto glow = [&](CGFloat x, CGFloat y, CGFloat rx, CGFloat ry, NSColor* color) {
        CGContextSaveGState(context_); CGContextTranslateCTM(context_, x, y); CGContextScaleCTM(context_, rx, ry);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        NSArray* colors = @[(__bridge id)color.CGColor, (__bridge id)Alpha(color,0).CGColor];
        const CGFloat locations[] = {0,.6};
        CGGradientRef gradient = CGGradientCreateWithColors(space, (__bridge CFArrayRef)colors, locations);
        if (gradient) { CGContextDrawRadialGradient(context_, gradient, CGPointZero, 0, CGPointZero, 1, kCGGradientDrawsAfterEndLocation); CGGradientRelease(gradient); }
        CGColorSpaceRelease(space); CGContextRestoreGState(context_);
    };
    glow(0,0,width*1.1,height*.45,Alpha(Down,.10)); glow(width,height,width*.9,height*.4,Alpha(Up,.09));
}
void Renderer::Panel(RectF r) {
    Rounded(r,14,Color(0xffffff,.035)); CGPathRef path = Path(r,14); CGContextAddPath(context_,path);
    CGContextSetStrokeColorWithColor(context_,Border.CGColor); CGContextSetLineWidth(context_,1); CGContextStrokePath(context_); CGPathRelease(path);
}
void Renderer::Bar(RectF r, CGFloat fraction, NSColor* from, NSColor* to) {
    Rounded(r,2,Track); if (fraction <= 0) return;
    r.right = r.left+(r.right-r.left)*std::clamp<CGFloat>(fraction,0,1); Gradient(r,from,to,false,2);
}
void Renderer::Bolt(CGFloat x, CGFloat y, CGFloat size, NSColor* color) {
    CGContextSaveGState(context_); CGContextTranslateCTM(context_,x,y); CGContextScaleCTM(context_,size/24,size/24);
    const CGPoint points[] = {{13,2},{4,14},{11,14},{10,22},{19,10},{12,10}};
    CGContextAddLines(context_, points, 6); CGContextClosePath(context_); CGContextSetFillColorWithColor(context_,color.CGColor);
    CGContextFillPath(context_); CGContextRestoreGState(context_);
}
void Renderer::Ring(const View& v, CGFloat x, CGFloat y) {
    const CGFloat cx=x+37,cy=y+37,r=31.08;
    CGContextSetLineWidth(context_,6.66); CGContextSetLineCap(context_,kCGLineCapRound);
    CGContextSetStrokeColorWithColor(context_,Track.CGColor); CGContextStrokeEllipseInRect(context_,CGRectMake(cx-r,cy-r,2*r,2*r));
    if (v.battery > 0) {
        CGContextBeginPath(context_); CGContextSetStrokeColorWithColor(context_,BatteryColor(v).CGColor);
        CGContextAddArc(context_,cx,cy,r,-M_PI_2,-M_PI_2+std::min(1.0f,v.battery)*2*M_PI,0); CGContextStrokePath(context_);
    }
    const CGFloat number=Measure(v.percent,20,true,true),unit=Measure(L"%",11,false,true),left=cx-(number+unit)/2;
    Text(v.percent,Rect(left,y+23,number+1,28),20,TextColor,true,true);
    Text(L"%",Rect(left+number,y+30,unit+1,17),11,Muted,false,true);
    if (v.mode==PowerMode::Charging) Bolt(cx-5,y+53,10,Charge);
}
void Renderer::ChartGrid(RectF r) { Line(r.left,std::floor((r.top+r.bottom)/2)+.5,r.right,std::floor((r.top+r.bottom)/2)+.5,Color(0xffffff,.05)); }
void Renderer::Chart(RectF r,const float* data,std::size_t count,NSColor* color,CGFloat maximum) {
    if (count<2 || maximum<=0) return;
    const CGFloat step=(r.right-r.left)/(count-1);
    auto point=[&](std::size_t i) { return CGPointMake(r.left+step*i,r.bottom-2-std::clamp<CGFloat>(data[i]/maximum,0,1)*(r.bottom-r.top-5)); };
    for (std::size_t begin=0; begin<count;) {
        while (begin<count && data[begin]<0) ++begin;
        std::size_t end=begin; while (end<count && data[end]>=0) ++end;
        if (end-begin>=2) {
            CGMutablePathRef stroke=CGPathCreateMutable(); auto a=point(begin); CGPathMoveToPoint(stroke,nullptr,a.x,a.y);
            for (auto i=begin+1;i<end;++i) { auto b=point(i); const CGFloat mid=(a.x+b.x)/2;
                CGPathAddCurveToPoint(stroke,nullptr,mid,a.y,mid,b.y,b.x,b.y); a=b; }
            CGMutablePathRef area=CGPathCreateMutableCopy(stroke);
            CGPathAddLineToPoint(area,nullptr,point(end-1).x,r.bottom); CGPathAddLineToPoint(area,nullptr,point(begin).x,r.bottom); CGPathCloseSubpath(area);
            CGContextSaveGState(context_); CGContextAddPath(context_,area); CGContextClip(context_);
            Gradient(r,Alpha(color,.28),Alpha(color,0),true); CGContextRestoreGState(context_);
            CGContextAddPath(context_,stroke); CGContextSetStrokeColorWithColor(context_,color.CGColor); CGContextSetLineWidth(context_,1.5); CGContextStrokePath(context_);
            CGPathRelease(area); CGPathRelease(stroke);
        }
        begin=end<count?end+1:count;
    }
}
void Renderer::ValueUnit(const std::wstring& value,const std::wstring& unit,CGFloat x,CGFloat y,CGFloat size,NSColor* color) {
    const CGFloat width=Measure(value,size,true,true); Text(value,Rect(x,y,width+1,size+6),size,color,true,true);
    if (value!=L"--") Text(unit,Rect(x+width+1,y+size-10,36,15),10.5,Muted,false,true);
}
void Renderer::Configure(const Components& components) {
    if (configured_ && components_.full==components.full && language_==i18n::CurrentLanguage()) return;
    components_=components; Sanitize(components_); language_=i18n::CurrentLanguage(); configured_=true; UpdateFullLayout();
}
// Port of src/ui_renderer.cpp Renderer::Full; all display text stays in ui::Model.
void Renderer::Full(CGContextRef context, const View& v) {
    context_ = context; Background(fullWidth_, Height());
    const auto layout = MakeFullLayout(components_, fullWidth_);
    const CGFloat content = fullWidth_ - 50, right = fullWidth_ - 25;
    const CGFloat half = (content + 10) / 2, quarter = (content + 10) / 4;
    const CGFloat inner = quarter - 10;
    const auto accent = components_.Enabled(FullComponent::Battery) ? Accent(v) : Ac;
    Rounded(Rect(11.5,17,12,12),6,Alpha(accent,.10));
    Rounded(Rect(14,19.5,7,7),3.5,accent);
    Text(T(Key::AppName), Rect(28, 10, 55, 26), 13.5, TextColor, true);
    Text(v.clock, Rect(fullWidth_ - 82, 10, 54, 26), 12.5, Muted, false, true, true);
    for (auto component : FullOrder) {
        if (!components_.Enabled(component)) continue;
        const auto card = layout.Card(component);
        switch (component) {
        case FullComponent::Hardware: {
            Panel(card);
            const CGFloat h = card.top;
            const std::wstring labels[] = {T(Key::Cpu), T(Key::Gpu), T(Key::Memory), T(Key::Processor)};
            const std::wstring values[] = {v.cpuValue, v.gpuUsage, v.memoryValue,
                                          v.packageW == L"--" ? L"--" : v.packageW + T(Key::WattSuffix)};
            const std::wstring details[] = {v.cpuW, v.gpuW, v.memW, v.packageNote};
            for (int i = 0; i < 4; ++i) {
                const CGFloat x = 25 + i * quarter;
                if (i) Line(x - 5, h + 14, x - 5, h + 82, Border);
                Text(labels[i], Rect(x, h + 10, inner, 16), 11, Muted);
                // Keep the package value and its unit inside its measured cell.
                CGFloat size = 20;
                const CGFloat width = Measure(values[i], size, true, true);
                if (width > inner - 1) size *= (inner - 1) / width;
                Text(values[i], Rect(x, h + 27, inner, 29), size, TextColor, true, true);
                Text(details[i], Rect(x, h + 58, inner, 16), 10.5, Muted, false, i != 3);
            }
            Bar(Rect(25, h + 81, inner, 3), v.cpu, Down, Color(0x22d3ee));
            Bar(Rect(25 + quarter, h + 81, inner, 3), v.gpu, Charge, Color(0x10b981));
            Bar(Rect(25 + quarter * 2, h + 81, inner, 3), v.memory, Up, Color(0xf0abfc));
            break;
        }
        case FullComponent::Battery: {
            Panel(card);
            const CGFloat b = card.top;
            const CGFloat saverWidth = v.saverLabel.empty() ? 0.0 : Measure(v.saverLabel, 11) + 8;
            // macOS has no power-plan row: reserve only the mode chip and saver tag.
            const CGFloat chipTextWidth = Measure(v.chip, 11), chipPadding = 16;
            const CGFloat chipWidth = chipTextWidth + chipPadding;
            Rounded(Rect(25, b + 10, chipWidth, 18), 9, Alpha(accent, .14));
            Text(v.chip, Rect(25 + chipPadding / 2, b + 10, chipTextWidth, 18), 11, accent);
            if (saverWidth > 0)
                Text(v.saverLabel, Rect(right - saverWidth, b + 10, saverWidth, 18), 11, accent, false, false, true);
            Ring(v, 25, b + 38);
            Text(v.etaLabel, Rect(113, b + 36, right - 113, 16), 11, Muted);
            if (!v.heroWord.empty()) {
                // Supplemental-power wording must fit the existing card, not widen all windows.
                Text(v.heroWord, Rect(113, b + 53, right - 113, 31),
                     v.supplemental ? 11.5 : 19.0, TextColor, true, false, false, v.supplemental);
            }
            else {
                CGFloat x = 113;
                for (const auto& part : v.hero) {
                    const CGFloat width = Measure(part.value, 25, true, true);
                    Text(part.value, Rect(x, b + 52, width + 1, 33), 25, accent, true, true);
                    x += width + 2;
                    const CGFloat uw = Measure(part.unit, 12);
                    Text(part.unit, Rect(x, b + 62, uw + 1, 20), 12, Muted);
                    x += uw + 7;
                }
            }
            const CGFloat metricWidth = (right - 113 + 6) / 3;
            Text(v.wattLabel, Rect(113, b + 87, metricWidth - 6, 16), 11, Muted);
            ValueUnit(v.watt, T(Key::Watt), 113, b + 103, 14, TextColor);
            Text(v.pphLabel, Rect(113 + metricWidth, b + 87, metricWidth - 6, 16), 11, Muted);
            ValueUnit(v.pph, L"%", 113 + metricWidth, b + 103, 14, TextColor);
            if (!v.eta20.empty()) {
                Text(T(Key::To20), Rect(113 + 2 * metricWidth, b + 87, metricWidth - 6, 16), 11, Muted);
                Text(v.eta20, Rect(113 + 2 * metricWidth, b + 103, metricWidth - 6, 20), 13, TextColor, true, true);
            }
            Text(v.foot, Rect(25, b + 129, content, 16), 11, Faint, false, true);
            break;
        }
        case FullComponent::PowerChart: {
            Panel(card);
            const CGFloat y = card.top;
            Text(T(Key::PowerChartPrefix) + v.powerLabel, Rect(25, y + 9, content * .60, 18), 11.5, Muted);
            Text(v.powerValue + T(Key::WattSuffix), Rect(25 + content * .60, y + 9, content * .40, 18), 12, TextColor, true, true, true);
            const auto chart = Rect(25, y + 34, content, 36);
            ChartGrid(chart);
            CGFloat maximum = 5;
            for (CGFloat value : v.powerHistory) maximum = std::max<CGFloat>(maximum, value);
            Chart(chart, v.powerHistory.data(), v.powerHistory.size(),
                  v.powerTrace == PowerTrace::BatteryWhole ? Drain : Ac, maximum * 1.15);
            break;
        }
        case FullComponent::Network: {
            Panel(card);
            const CGFloat y = card.top;
            Text(T(Key::Network), Rect(25, y + 9, networkLabelWidth_, 17), 11.5, Muted);
            Text(v.totals, Rect(29 + networkLabelWidth_, y + 9, content - networkLabelWidth_ - 4, 17), 10.5, Faint, false, true, true);
            auto speed = [&](const Speed& s, CGFloat x, const wchar_t* arrow, NSColor* color) {
                Text(arrow, Rect(x, y + 31, 12, 26), 13, color, true);
                const CGFloat width = std::min(74.0, Measure(s.value, 19, true, true));
                Text(s.value, Rect(x + 13, y + 30, width + 1, 28), 19, TextColor, true, true);
                Text(s.unit, Rect(x + 17 + width, y + 38, 42, 18), 10.5, Muted, false, true);
            };
            speed(v.up, 25, L"↑", Up);
            speed(v.down, 25 + half, L"↓", Down);
            Line(25, y + 64, right, y + 64, Border);
            const auto color = NetworkColor(v.netState);
            Text(T(Key::Latency), Rect(25, y + 77, latencyLabelWidth_, 24), 11.5, Muted);
            Text(v.latency, Rect(34 + latencyLabelWidth_, y + 73, content - latencyLabelWidth_ - 9, 32), 23, color, true, true);
            Text(v.netHint, Rect(25, y + 110, content, 36), 11, color, false, false, false, true);
            break;
        }
        case FullComponent::Processes: {
            Panel(card);
            const CGFloat ptop = card.top;
            Text(T(Key::TopProcesses), Rect(25, ptop + 9, content * .55, 17), 11.5, Muted);
            Text(v.selfCpu, Rect(25 + content * .55, ptop + 9, content * .45, 17), 11, Faint, false, true, true);
            if (v.processes.empty()) Text(T(Key::Counting), Rect(25, ptop + 33, content, 19), 12, Faint);
            for (size_t i = 0; i < v.processes.size(); ++i) {
                const auto& p = v.processes[i];
                const CGFloat y = ptop + 33 + static_cast<CGFloat>(i) * 19;
                Text(p.name, Rect(25, y, content - 128, 19), 12, TextColor);
                Bar(Rect(right - 120, y + 7.5, 70, 4), p.fraction, Color(0xf59e0b), Drain);
                Text(p.percent, Rect(right - 42, y, 42, 19), 12, Muted, false, true, true);
            }
            break;
        }
        case FullComponent::DiskIO: {
            Panel(card);
            const CGFloat y = card.top;
            Text(T(Key::DiskIO), Rect(25, y + 9, content, 18), 11.5, Muted);
            Text(T(Key::Read), Rect(25, y + 37, readLabelWidth_, 24), 11, Down);
            Text(v.read.value + L" " + v.read.unit, Rect(25 + readLabelWidth_, y + 35, half - readLabelWidth_ - 5, 28), 15, TextColor, true, true);
            Text(T(Key::Write), Rect(25 + half, y + 37, writeLabelWidth_, 24), 11, Up);
            Text(v.write.value + L" " + v.write.unit, Rect(25 + half + writeLabelWidth_, y + 35, content - half - writeLabelWidth_, 28), 15, TextColor, true, true);
            break;
        }
        case FullComponent::SystemDisk: {
            Panel(card);
            const CGFloat y = card.top;
            Text(T(Key::SystemDrive), Rect(25, y + 9, content - 125, 18), 11.5, Muted);
            Text(v.diskPercent, Rect(right - 117, y + 7, 117, 24), 17, v.disk >= .9 ? Danger : TextColor, true, true, true);
            Bar(Rect(25, y + 38, content, 5), v.disk, Down, Up);
            Text(T(Key::FreePrefix) + v.diskFree, Rect(25, y + 53, content, 18), 11.5, Muted, false, true);
            break;
        }
        case FullComponent::SystemInfo: {
            Panel(card);
            const CGFloat y = card.top;
            Text(T(Key::SystemInfo), Rect(25, y + 9, content, 18), 11.5, Muted);
            auto row = [&](const std::wstring& label, const std::wstring& value, CGFloat top) {
                Text(label, Rect(25, y + top, systemLabelWidth_, 18), 11, Faint);
                Text(value, Rect(25 + systemLabelWidth_, y + top, content - systemLabelWidth_, 18), 12, TextColor, false, true, true);
            };
            row(T(Key::MemoryUsedTotal), v.memoryDetail, 33);
            row(T(Key::ProcessCount), v.processCount, 51);
            row(T(Key::Uptime), v.uptime, 69);
            break;
        }
        case FullComponent::BatteryHealth: {
            Panel(card);
            const CGFloat y = card.top;
            Text(T(Key::BatteryHealth), Rect(25, y + 9, half - 8, 18), 11.5, Muted);
            Text(T(Key::CycleCount), Rect(25 + half, y + 9, content - half, 18), 11.5, Muted);
            Text(v.health, Rect(25, y + 34, half - 8, 30), 21, Charge, true, true);
            Text(v.cycles, Rect(25 + half, y + 34, content - half, 30), 21, TextColor, true, true);
            break;
        }
        default: break;
        }
    }
    context_ = nullptr;
}

void Renderer::UpdateFullLayout() {
    // Text measurement happens on configuration/language changes, never in mini paints.
    networkLabelWidth_ = Measure(T(Key::Network), 11.5) + 4;
    latencyLabelWidth_ = Measure(T(Key::Latency), 11.5) + 4;
    readLabelWidth_ = Measure(T(Key::Read), 11) + 6;
    writeLabelWidth_ = Measure(T(Key::Write), 11) + 6;
    systemLabelWidth_ = 84;
    for (Key key : {Key::CpuFrequency, Key::MemoryUsedTotal, Key::ProcessCount, Key::Uptime})
        systemLabelWidth_ = std::max(systemLabelWidth_, Measure(T(key), 11) + 12);
    CGFloat chip = 0;
    for (Key key : {Key::LowBattery, Key::Discharging, Key::Charging, Key::OnAc, Key::NoBattery, Key::Reading,
                    Key::AssistChip, Key::NotCharging})
        chip = std::max(chip, Measure(T(key), 11) + 16);
    // Do not reserve width for the shared model's unsupported power-plan text.
    fullWidth_ = std::max(320.0, 50 + chip + 8 + Measure(T(Key::BatterySaverShort), 11) + 8);
    CGFloat hardware = 60;
    for (Key key : {Key::Cpu, Key::Gpu, Key::Memory, Key::Processor})
        hardware = std::max(hardware, Measure(T(key), 11) + 2);
    hardware = std::max(hardware, Measure(T(Key::WholeChip), 10.5) + 2);
    for (Key key : {Key::MeterWarmup, Key::MeterNoCounter, Key::MeterNoDomain, Key::MeterReadFailed})
        hardware = std::max(hardware, std::max(Measure(T(key), 10.5),
            Measure(T(key), 10.5, false, true)) + 2);
    hardware = std::max(hardware, Measure(L"999.9" + T(Key::WattSuffix), 10.5) + 2);
    for (const auto& text : {T(Key::DgpuPrefix) + L"9.9" + T(Key::WattSuffix),
                             T(Key::DgpuPrefix) + L"999" + T(Key::WattSuffix), T(Key::DgpuSleeping),
                             T(Key::DgpuPrefix) + L"--"})
        hardware = std::max(hardware, std::max(Measure(text, 10.5),
            Measure(text, 10.5, false, true)) + 2);
    fullWidth_ = std::max(fullWidth_, 50 + hardware * 4 + 30);
    fullWidth_ = std::max(fullWidth_, 50 + systemLabelWidth_ + Measure(L"999.9 / 999.9" + T(Key::GbSuffix), 12));
    for (Key key : {Key::FullyCharged, Key::AcPower, Key::NoBatteryDetected, Key::Calculating, Key::Reading,
                    Key::NotCharging})
        fullWidth_ = std::max(fullWidth_, 138 + Measure(T(key), 19, true));
    for (Key key : {Key::SinceLaunch, Key::SinceUnplugged})
        fullWidth_ = std::max(fullWidth_, 50 + Measure(T(key) + L"99+ " + T(Key::Hours) +
            T(Key::AveragePrefix) + L"999.9" + T(Key::WattSuffix), 11));
    fullWidth_ = std::ceil(fullWidth_);
}

CGFloat Renderer::ToastHeight(const std::wstring& message, CGFloat width) const {
    CTFramesetterRef framesetter=CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)Attributed(message,Font(12),TextColor));
    const CGSize size=CTFramesetterSuggestFrameSizeWithConstraints(framesetter,CFRangeMake(0,0),nullptr,CGSizeMake(width-50,10000),nullptr);
    CFRelease(framesetter); return std::ceil(std::max<CGFloat>(96,size.height+65));
}
void Renderer::Toast(CGContextRef context, NSSize size, const NetEvent& event, const std::wstring& message) {
    context_=context; Background(size.width,size.height);
    NSColor* color=event.recovered?Charge:Danger;
    Rounded(Rect(14,16,3,size.height-32),1.5,color);
    Text(T(event.recovered?Key::NetworkRestored:Key::NetworkDisconnected),Rect(25,12,size.width-48,24),13.5,color,true);
    Text(message,Rect(25,41,size.width-50,size.height-65),12,TextColor,false,false,false,true);
    Text(T(Key::AppName),Rect(25,size.height-24,size.width-50,16),10,Faint,false,false,true);
    context_=nullptr;
}
} // namespace pulse::mac
