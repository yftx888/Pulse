#include "ui_renderer.h"
#include <d2d1helper.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pulse::ui {
namespace {
D2D1_COLOR_F Color(unsigned rgb, float alpha = 1) {
    return D2D1::ColorF(rgb, alpha);
}
const auto TextColor = Color(0xe9edf6), Muted = Color(0x8b94aa), Faint = Color(0x586079);
const auto Down = Color(0x38bdf8), Up = Color(0xa78bfa), Drain = Color(0xfbbf24);
const auto Charge = Color(0x34d399), Ac = Color(0x60a5fa), Danger = Color(0xf87171);
const auto Track = Color(0xffffff, .07f), Border = Color(0xffffff, .07f);
D2D1_COLOR_F Alpha(D2D1_COLOR_F color, float alpha) { color.a = alpha; return color; }
D2D1_COLOR_F Accent(const View& v) {
    if (v.low) return Danger;
    if (v.mode == PowerMode::Charging) return Charge;
    if (v.mode == PowerMode::AcIdle || v.mode == PowerMode::NoBattery) return Ac;
    return Drain;
}
D2D1_COLOR_F BatteryColor(const View& v) {
    return v.battery < 0 ? Faint : v.battery <= .20f ? Danger : v.battery <= .45f ? Drain : Charge;
}
D2D1_COLOR_F NetworkColor(NetState state) {
    return state == NetState::Online ? Charge : state == NetState::Slow ? Drain :
           state == NetState::Offline ? Danger : Faint;
}
D2D1_COLOR_F GpuTempColor(const View& v, D2D1_COLOR_F normal) {
    return v.gpuTempLevel >= 2 ? Danger : v.gpuTempLevel == 1 ? Drain : normal;
}
D2D1_RECT_F Rect(float x, float y, float width, float height) {
    return D2D1::RectF(x, y, x + width, y + height);
}
bool Contains(D2D1_RECT_F r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
} // namespace

Layout FullLayout(const Components& components, float width) {
    Layout layout;
    float y = 44;
    bool any = false;
    for (auto component : FullOrder) {
        if (!components.Enabled(component)) continue;
        const auto index = static_cast<size_t>(component);
        if (any) y += 8;
        const float height = FullComponents[index].extent;
        layout.cards[index] = Rect(12, y, width - 24, height);
        y += height;
        any = true;
    }
    layout.height = y + 12;
    return layout;
}

void StyleWindow(HWND window) {
    // Keep the module loaded while these cached function pointers are in use.
    static const HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    using SetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    using ExtendFrameFn = HRESULT(WINAPI*)(HWND, const MARGINS*);
    static const auto setWindowAttribute = reinterpret_cast<SetWindowAttributeFn>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    static const auto extendFrame = reinterpret_cast<ExtendFrameFn>(
        GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
    const BOOL dark = TRUE;
    const BOOL noTransitions = TRUE;
    const int round = 2; // DWMWCP_ROUND; numeric values also support older MinGW headers.
    const COLORREF border = RGB(0x28, 0x34, 0x45);
    if (setWindowAttribute) {
        // DWMWA_USE_IMMERSIVE_DARK_MODE is 19 on older Windows 10 builds.
        if (FAILED(setWindowAttribute(window, 20, &dark, sizeof(dark))))
            setWindowAttribute(window, 19, &dark, sizeof(dark));
        setWindowAttribute(window, 3, &noTransitions, sizeof(noTransitions));
        setWindowAttribute(window, 33, &round, sizeof(round));
        setWindowAttribute(window, 34, &border, sizeof(border));
    }
    const MARGINS shadow{1, 1, 1, 1};
    if (extendFrame) extendFrame(window, &shadow);
}
UINT WindowDpi(HWND window) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    const auto getDpi = reinterpret_cast<GetDpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (getDpi) { const UINT dpi = getDpi(window); if (dpi) return dpi; }
    return MonitorDpi(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
}
UINT MonitorDpi(HMONITOR monitor) {
    using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    UINT x = 96, y = 96;
    if (shcore) {
        const auto getDpi = reinterpret_cast<GetDpiFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
        if (!getDpi || FAILED(getDpi(monitor, 0, &x, &y))) x = 96;
        FreeLibrary(shcore);
    } else {
        HDC dc = GetDC(nullptr);
        if (dc) { x = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(nullptr, dc); }
    }
    return x ? x : 96;
}

HRESULT Renderer::Initialize(bool fullPanel) {
    fullPanel_ = fullPanel;
    struct Shared {
        ComPtr<ID2D1Factory> factory;
        ComPtr<IDWriteFactory> write;
        ComPtr<IDWriteTypography> tabular;
        ComPtr<ID2D1StrokeStyle> stroke;
        std::wstring font, numericFont;
        HRESULT result = E_FAIL;
        Shared() {
            result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.Put());
            if (FAILED(result)) return;
            result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(write.Put()));
            if (FAILED(result)) return;
            ComPtr<IDWriteFontCollection> collection;
            result = write->GetSystemFontCollection(collection.Put());
            if (FAILED(result)) return;
            auto available = [&](const wchar_t* name) {
                UINT32 index = 0; BOOL exists = FALSE;
                return SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
            };
            font = available(L"Segoe UI Variable") ? L"Segoe UI Variable" :
                available(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Microsoft YaHei UI";
            numericFont = available(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : font;
            result = write->CreateTypography(tabular.Put());
            if (FAILED(result)) return;
            result = tabular->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            if (FAILED(result)) return;
            auto properties = D2D1::StrokeStyleProperties();
            properties.startCap = properties.endCap = properties.dashCap = D2D1_CAP_STYLE_ROUND;
            properties.lineJoin = D2D1_LINE_JOIN_ROUND;
            result = factory->CreateStrokeStyle(properties, nullptr, 0, stroke.Put());
        }
    };
    static Shared shared; // All renderers live on the UI thread.
    if (FAILED(shared.result)) return shared.result;
    factory_.Retain(shared.factory.Get()); write_.Retain(shared.write.Get());
    tabular_.Retain(shared.tabular.Get()); roundStroke_.Retain(shared.stroke.Get());
    font_ = shared.font; numericFont_ = shared.numericFont;
    HRESULT hr = S_OK;
    auto* format = Format(12, false, false);
    if (!format) return E_FAIL;
    hr = write_->CreateEllipsisTrimmingSign(format, ellipsis_.Put());
    if (SUCCEEDED(hr) && fullPanel_) UpdateFullLayout();
    return hr;
}
void Renderer::SetSoftware(bool software) {
    if (software_ == software) return;
    software_ = software;
    DiscardTarget();
}
void Renderer::SetWindow(HWND window, UINT dpi) { window_ = window; SetDpi(dpi); }
void Renderer::SetDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    if (target_) target_->SetDpi(RenderDpi(), RenderDpi());
}
void Renderer::SetZoom(int percent) {
    percent = std::clamp(percent, 80, 200);
    if (zoom_ == percent) return;
    zoom_ = percent;
    // Keep all layout coordinates in logical DIPs. The target's effective DPI
    // scales text, strokes, geometry and brushes together without a bitmap pass.
    DiscardTarget();
    layouts_.clear();
    formats_.clear();
    layoutSerial_ = 0;
    ellipsis_.Reset();
    if (write_) {
        if (auto* format = Format(12, false, false))
            write_->CreateEllipsisTrimmingSign(format, ellipsis_.Put());
        if (compact_) UpdateCompactLayout();
    }
}
void Renderer::LanguageChanged() {
    layouts_.clear();
    formats_.clear();
    layoutSerial_ = 0;
    ellipsis_.Reset();
    if (!write_) return;
    if (auto* format = Format(12, false, false))
        write_->CreateEllipsisTrimmingSign(format, ellipsis_.Put());
    if (fullPanel_) UpdateFullLayout();
    if (compact_) UpdateCompactLayout();
}
void Renderer::SetComponents(const Components& components, bool compact) {
    const bool chart = !compact && components.Enabled(FullComponent::PowerChart);
    if (chartVisible_ && !chart) {
        gradients_.clear();
        layouts_.clear(); // Release chart brushes/layouts; rebuild visible text through the same LRU cache.
    }
    chartVisible_ = chart;
    compact_ = compact;
    components_ = components;
    if (compact) UpdateCompactLayout();
    else UpdateFullLayout();
}
bool Renderer::SetCompactEtaVisible(bool visible) {
    if (compactEtaVisible_ == visible) return false;
    compactEtaVisible_ = visible;
    if (!compact_ || !components_.Enabled(CompactComponent::Eta)) return false;
    UpdateCompactLayout();
    return true;
}
void Renderer::Resize() {
    if (!target_) return;
    RECT r{};
    GetClientRect(window_, &r);
    if (r.right > 0 && r.bottom > 0 && FAILED(target_->Resize(D2D1::SizeU(r.right, r.bottom)))) DiscardTarget();
}
void Renderer::DiscardTarget() {
    gradients_.clear();
    downGlow_.Reset(); upGlow_.Reset();
    brush_.Reset(); target_.Reset();
}
bool Renderer::CanRender() const {
    return window_ && IsWindowVisible(window_) && !IsIconic(window_) &&
        (!target_ || !(target_->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED));
}
HRESULT Renderer::CreateTarget() {
    if (target_) return S_OK;
    RECT r{};
    GetClientRect(window_, &r);
    if (r.right <= 0 || r.bottom <= 0) return E_FAIL;
    auto properties = D2D1::RenderTargetProperties(software_ ?
        D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT);
    properties.dpiX = properties.dpiY = RenderDpi();
    HRESULT hr = factory_->CreateHwndRenderTarget(properties,
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(r.right, r.bottom)), target_.Put());
    if (FAILED(hr)) return hr;
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    hr = target_->CreateSolidColorBrush(TextColor, brush_.Put());
    if (FAILED(hr)) DiscardTarget();
    return hr;
}
HRESULT Renderer::Finish() {
    const HRESULT hr = target_->EndDraw();
    // Consume the previous presentation's occlusion flag. Future checks then
    // describe current visibility instead of reporting a stale occlusion once.
    (void)target_->CheckWindowState();
    if (FAILED(hr)) DiscardTarget();
    return hr;
}
IDWriteTextFormat* Renderer::Format(float size, bool bold, bool numeric) {
    const FormatKey key{static_cast<int>(std::round(size * 10)), bold ? 600 : 400, numeric};
    auto found = formats_.find(key);
    if (found != formats_.end()) return found->second.Get();
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(write_->CreateTextFormat((numeric ? numericFont_ : font_).c_str(), nullptr,
        bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, i18n::LocaleName(), format.Put()))) return nullptr;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return formats_.emplace(key, std::move(format)).first->second.Get();
}
IDWriteTextLayout* Renderer::TextLayout(const std::wstring& text, float size, bool bold, bool numeric,
                                      float width, float height, bool right, bool wrap) {
    if (text.empty() || width <= 0 || height <= 0) return nullptr;
    const FormatKey formatKey{static_cast<int>(std::round(size * 10)), bold ? 600 : 400, numeric};
    LayoutKey key{text, formatKey, width, height, right, wrap};
    auto found = layouts_.find(key);
    if (found != layouts_.end()) {
        found->second.used = ++layoutSerial_;
        return found->second.layout.Get();
    }
    auto* format = Format(size, bold, numeric);
    if (!format) return nullptr;
    CachedLayout entry;
    if (FAILED(write_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                       width, height, entry.layout.Put()))) return nullptr;
    entry.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    entry.layout->SetTextAlignment(right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    entry.layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        entry.layout->SetTrimming(&trimming, ellipsis_.Get());
    }
    if (numeric) entry.layout->SetTypography(tabular_.Get(), {0, static_cast<UINT32>(text.size())});
    entry.used = ++layoutSerial_;
    if (layouts_.size() >= 256) {
        const auto oldest = std::min_element(layouts_.begin(), layouts_.end(), [](const auto& a, const auto& b) {
            return a.second.used < b.second.used;
        });
        layouts_.erase(oldest);
    }
    return layouts_.emplace(std::move(key), std::move(entry)).first->second.layout.Get();
}
float Renderer::Measure(const std::wstring& text, float size, bool bold, bool numeric) {
    auto* layout = TextLayout(text, size, bold, numeric, 10000, 100, false, false);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}
void Renderer::Text(const std::wstring& text, D2D1_RECT_F rect, float size, D2D1_COLOR_F color,
                    bool bold, bool numeric, bool right, bool wrap) {
    auto* layout = TextLayout(text, size, bold, numeric, rect.right - rect.left, rect.bottom - rect.top, right, wrap);
    if (!layout) return;
    brush_->SetColor(color);
    target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Renderer::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width) {
    brush_->SetColor(color);
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), width, roundStroke_.Get());
}
void Renderer::Rounded(D2D1_RECT_F r, float radius, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
}
ID2D1LinearGradientBrush* Renderer::LinearGradient(D2D1_COLOR_F from, D2D1_COLOR_F to) {
    const std::array<float, 8> key{from.r, from.g, from.b, from.a, to.r, to.g, to.b, to.a};
    auto found = gradients_.find(key);
    if (found != gradients_.end()) return found->second.Get();
    ComPtr<ID2D1GradientStopCollection> stops;
    const D2D1_GRADIENT_STOP values[] = {{0, from}, {1, to}};
    if (FAILED(target_->CreateGradientStopCollection(values, 2, stops.Put()))) return nullptr;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(0, 0), D2D1::Point2F(1, 1)), stops.Get(), gradient.Put()))) return nullptr;
    return gradients_.emplace(key, std::move(gradient)).first->second.Get();
}
void Renderer::Gradient(D2D1_RECT_F r, D2D1_COLOR_F from, D2D1_COLOR_F to, bool vertical, float radius) {
    auto* gradient = LinearGradient(from, to);
    if (!gradient) { Rounded(r, radius, from); return; }
    gradient->SetStartPoint(D2D1::Point2F(r.left, r.top));
    gradient->SetEndPoint(vertical ? D2D1::Point2F(r.left, r.bottom) : D2D1::Point2F(r.right, r.top));
    target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gradient);
}
void Renderer::Background(float width, float height) {
    target_->Clear(Color(0x0c111d));
    if (auto* gradient = LinearGradient(Color(0x151c2f), Color(0x0c111d))) {
        gradient->SetStartPoint(D2D1::Point2F(0, 0));
        gradient->SetEndPoint(D2D1::Point2F(width * .36f, height));
        target_->FillRectangle(Rect(0, 0, width, height), gradient);
    }
    auto glow = [&](ComPtr<ID2D1RadialGradientBrush>& radial, D2D1_POINT_2F center,
                    float rx, float ry, D2D1_COLOR_F color) {
        if (!radial) {
            const D2D1_GRADIENT_STOP gs[] = {{0, color}, {.6f, Alpha(color, 0)}, {1, Alpha(color, 0)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            if (FAILED(target_->CreateGradientStopCollection(gs, 3, collection.Put())) ||
                FAILED(target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                    center, D2D1::Point2F(0, 0), rx, ry), collection.Get(), radial.Put()))) return;
        }
        radial->SetCenter(center); radial->SetRadiusX(rx); radial->SetRadiusY(ry);
        target_->FillRectangle(Rect(0, 0, width, height), radial.Get());
    };
    glow(downGlow_, D2D1::Point2F(0, 0), width * 1.1f, height * .45f, Alpha(Down, .10f));
    glow(upGlow_, D2D1::Point2F(width, height), width * .9f, height * .4f, Alpha(Up, .09f));
}
void Renderer::Panel(D2D1_RECT_F r) {
    Rounded(r, 14, Color(0xffffff, .035f));
    brush_->SetColor(Border);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(r, 14, 14), brush_.Get(), 1);
}
void Renderer::Bar(D2D1_RECT_F r, float fraction, D2D1_COLOR_F from, D2D1_COLOR_F to) {
    Rounded(r, 2, Track);
    if (fraction <= 0) return;
    r.right = r.left + (r.right - r.left) * std::clamp(fraction, 0.0f, 1.0f);
    Gradient(r, from, to, false, 2);
}
void Renderer::Bolt(float x, float y, float size, D2D1_COLOR_F color) {
    if (!bolt_) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(path.Put())) || FAILED(path->Open(sink.Put()))) return;
        sink->BeginFigure(D2D1::Point2F(13, 2), D2D1_FIGURE_BEGIN_FILLED);
        const D2D1_POINT_2F pts[] = {{4,14}, {11,14}, {10,22}, {19,10}, {12,10}};
        sink->AddLines(pts, 5);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        bolt_ = std::move(path); // Device-independent; no geometry allocation on later frames.
    }
    D2D1::Matrix3x2F previous;
    target_->GetTransform(&previous);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(size / 24, size / 24) *
                          D2D1::Matrix3x2F::Translation(x, y) * previous);
    brush_->SetColor(color);
    target_->FillGeometry(bolt_.Get(), brush_.Get());
    target_->SetTransform(previous);
}
void Renderer::Ring(const View& v, float x, float y) {
    const auto center = D2D1::Point2F(x + 37, y + 37);
    constexpr float radius = 31.08f, stroke = 6.66f;
    brush_->SetColor(Track);
    target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush_.Get(), stroke);
    brush_->SetColor(BatteryColor(v));
    if (v.battery >= .9999f) target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush_.Get(), stroke);
    else if (v.battery > 0) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory_->CreatePathGeometry(path.Put())) && SUCCEEDED(path->Open(sink.Put()))) {
            const float angle = v.battery * 6.28318530718f - 1.57079632679f;
            sink->BeginFigure(D2D1::Point2F(center.x, center.y - radius), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)),
                D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                v.battery > .5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) target_->DrawGeometry(path.Get(), brush_.Get(), stroke, roundStroke_.Get());
        }
    }
    const float numberWidth = Measure(v.percent, 20, true, true);
    const float unitWidth = Measure(L"%", 11, false, true);
    const float left = center.x - (numberWidth + unitWidth) / 2;
    Text(v.percent, Rect(left, y + 23, numberWidth + 1, 28), 20, TextColor, true, true);
    Text(L"%", Rect(left + numberWidth, y + 30, unitWidth + 1, 17), 11, Muted, false, true);
    if (v.mode == PowerMode::Charging) Bolt(center.x - 5, y + 53, 10, Charge);
}
D2D1_RECT_F Renderer::ButtonRect(Button b, bool compact, float width) const {
    if (compact) return b == Button::Mode ? Rect(width - 30, 8, 24, 24) : Rect(0, 0, 0, 0);
    const int index = b == Button::Mode ? 0 : b == Button::Pin ? 1 : b == Button::Minimize ? 2 : b == Button::Close ? 3 : -1;
    return index < 0 ? Rect(0, 0, 0, 0) : Rect(width - 111 + index * 25.0f, 11, 24, 24);
}
Button Renderer::HitTest(float x, float y, bool compact, float width) const {
    for (auto b : {Button::Mode, Button::Pin, Button::Minimize, Button::Close})
        if (Contains(ButtonRect(b, compact, width), x, y)) return b;
    return Button::None;
}
void Renderer::Buttons(bool compact, bool pinned, Button hover, Button pressed, float width, D2D1_COLOR_F accent) {
    for (auto b : {Button::Mode, Button::Pin, Button::Minimize, Button::Close}) {
        auto r = ButtonRect(b, compact, width);
        if (r.right <= r.left) continue;
        auto color = b == Button::Pin && pinned ? accent : Faint;
        if (b == hover || b == pressed) {
            Rounded(r, 7, b == Button::Close ? Alpha(Danger, .18f) : Color(0xffffff, .08f));
            color = b == Button::Close ? Danger : TextColor;
        }
        const float x = r.left + 5, y = r.top + 5, s = 14;
        auto line = [&](float x1, float y1, float x2, float y2) {
            Line(x + x1 / 24 * s, y + y1 / 24 * s, x + x2 / 24 * s, y + y2 / 24 * s, color, 1.4f);
        };
        if (b == Button::Close) { line(6,6,18,18); line(18,6,6,18); }
        else if (b == Button::Minimize) line(5,12,19,12);
        else if (b == Button::Pin) {
            line(8,2,16,2); line(8,2,8,6); line(16,2,16,6); line(8,6,9,7);
            line(16,6,15,7); line(9,7,9,11); line(15,7,15,11);
            line(9,11,5,15); line(15,11,19,15); line(5,15,5,17);
            line(19,15,19,17); line(5,17,19,17); line(12,17,12,22);
        } else if (taskbarDetails_ && b == Button::Mode) {
            line(5,8,12,15); line(12,15,19,8);
        } else if (compact) {
            line(15,3,21,3); line(21,3,21,9); line(9,21,3,21); line(3,21,3,15);
            line(21,3,14,10); line(3,21,10,14);
        } else {
            line(4,14,10,14); line(10,14,10,20); line(20,10,14,10); line(14,10,14,4);
            line(14,10,21,3); line(3,21,10,14);
        }
    }
}
void Renderer::ChartGrid(D2D1_RECT_F r) {
    const float mid = std::floor((r.top + r.bottom) / 2) + .5f;
    Line(r.left, mid, r.right, mid, Color(0xffffff, .05f));
}
void Renderer::Chart(D2D1_RECT_F r, const float* data, size_t count, D2D1_COLOR_F color, float maximum) {
    if (count < 2 || maximum <= 0) return;
    const float step = (r.right - r.left) / static_cast<float>(count - 1);
    auto point = [&](size_t i) {
        return D2D1::Point2F(r.left + step * static_cast<float>(i),
            r.bottom - 2 - std::clamp(data[i] / maximum, 0.0f, 1.0f) * (r.bottom - r.top - 5));
    };
    for (size_t begin = 0; begin < count;) {
        while (begin < count && data[begin] < 0) ++begin;
        size_t end = begin;
        while (end < count && data[end] >= 0) ++end;
        if (end - begin >= 2) {
            auto geometry = [&](bool filled, ComPtr<ID2D1PathGeometry>& result) {
                ComPtr<ID2D1GeometrySink> sink;
                if (FAILED(factory_->CreatePathGeometry(result.Put())) || FAILED(result->Open(sink.Put()))) return false;
                sink->BeginFigure(point(begin), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                for (size_t i = begin + 1; i < end; ++i) {
                    const auto a = point(i - 1), b = point(i);
                    const float mid = (a.x + b.x) / 2;
                    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(mid, a.y), D2D1::Point2F(mid, b.y), b));
                }
                if (filled) {
                    sink->AddLine(D2D1::Point2F(point(end - 1).x, r.bottom));
                    sink->AddLine(D2D1::Point2F(point(begin).x, r.bottom));
                }
                sink->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
                return SUCCEEDED(sink->Close());
            };
            ComPtr<ID2D1PathGeometry> area, stroke;
            if (geometry(true, area)) {
                if (auto* gradient = LinearGradient(Alpha(color, .28f), Alpha(color, 0))) {
                    gradient->SetStartPoint(D2D1::Point2F(r.left, r.top));
                    gradient->SetEndPoint(D2D1::Point2F(r.left, r.bottom));
                    target_->FillGeometry(area.Get(), gradient);
                }
            }
            if (geometry(false, stroke)) {
                brush_->SetColor(color);
                target_->DrawGeometry(stroke.Get(), brush_.Get(), 1.5f, roundStroke_.Get());
            }
        }
        begin = end < count ? end + 1 : count;
    }
}
void Renderer::ValueUnit(const std::wstring& value, const std::wstring& unit, float x, float y,
                         float size, D2D1_COLOR_F color) {
    const float width = Measure(value, size, true, true);
    Text(value, Rect(x, y, width + 1, size + 6), size, color, true, true);
    if (value != L"--") Text(unit, Rect(x + width + 1, y + size - 10, 36, 15), 10.5f, Muted, false, true);
}
void Renderer::Full(const View& v, bool pinned, Button hover, Button pressed) {
    const auto layout = FullLayout(components_, fullWidth_);
    const float content = fullWidth_ - 50, right = fullWidth_ - 25;
    const float half = (content + 10) / 2, quarter = (content + 10) / 4;
    const float inner = quarter - 10;
    const auto accent = components_.Enabled(FullComponent::Battery) ? Accent(v) : Ac;
    brush_->SetColor(Alpha(accent, .10f));
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(17.5f, 23), 6, 6), brush_.Get());
    brush_->SetColor(accent);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(17.5f, 23), 3.5f, 3.5f), brush_.Get());
    Text(T(Key::AppName), Rect(28, 10, 55, 26), 13.5f, TextColor, true);
    Text(v.clock, Rect(fullWidth_ - 177, 10, 54, 26), 12.5f, Muted, false, true, true);
    Buttons(false, pinned, hover, pressed, target_->GetSize().width, accent);

    const float viewport = target_->GetSize().height;
    target_->PushAxisAlignedClip(Rect(0, 44, fullWidth_, std::max(0.0f, viewport - 44)), D2D1_ANTIALIAS_MODE_ALIASED);
    target_->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll_));
    for (auto component : FullOrder) {
        if (!components_.Enabled(component)) continue;
        const auto card = layout.Card(component);
        if (card.bottom - scroll_ < 44 || card.top - scroll_ > viewport) continue;
        switch (component) {
        case FullComponent::Hardware: {
            Panel(card);
            const float h = card.top;
            const std::wstring labels[] = {T(Key::Cpu), T(Key::Gpu), T(Key::Memory), T(Key::Processor)};
            const std::wstring values[] = {v.cpuValue, v.gpuUsage, v.memoryValue,
                                          v.packageW == L"--" ? L"--" : v.packageW + T(Key::WattSuffix)};
            const std::wstring watts[] = {v.cpuW, v.gpuW, v.memW};
            for (int i = 0; i < 4; ++i) {
                const float x = 25 + i * quarter;
                if (i) Line(x - 5, h + 14, x - 5, h + 82, Border);
                Text(labels[i], Rect(x, h + 10, inner, 16), 11, Muted);
                // Keep the package value and its unit inside its measured cell.
                float size = 20;
                const float width = Measure(values[i], size, true, true);
                if (width > inner - 1) size *= (inner - 1) / width;
                Text(values[i], Rect(x, h + 27, inner, 29), size, TextColor, true, true);
                const std::wstring detail = i == 3 ? T(Key::WholeChip) :
                    watts[i] == L"--" ? L"--" : watts[i] + T(Key::WattSuffix);
                Text(detail, Rect(x, h + 58, inner, 16), 10.5f, Muted, false, i != 3);
            }
            Bar(Rect(25, h + 81, inner, 3), v.cpu, Down, Color(0x22d3ee));
            Bar(Rect(25 + quarter, h + 81, inner, 3), v.gpu, Charge, Color(0x10b981));
            Bar(Rect(25 + quarter * 2, h + 81, inner, 3), v.memory, Up, Color(0xf0abfc));

            break;
        }
        case FullComponent::Battery: {
            Panel(card);
            const float b = card.top;
            const float chipWidth = Measure(v.chip, 11) + 16;
            Rounded(Rect(25, b + 10, chipWidth, 18), 9, Alpha(accent, .14f));
            brush_->SetColor(Alpha(accent, .28f));
            target_->DrawRoundedRectangle(D2D1::RoundedRect(Rect(25, b + 10, chipWidth, 18), 9, 9), brush_.Get(), 1);
            Text(v.chip, Rect(33, b + 10, chipWidth - 16, 18), 11, accent);
            Text(v.scheme, Rect(25 + chipWidth + 8, b + 10, content - chipWidth - 8, 18), 11, Faint, false, false, true);
            Ring(v, 25, b + 38);
            Text(v.etaLabel, Rect(113, b + 36, right - 113, 16), 11, Muted);
            if (!v.heroWord.empty()) Text(v.heroWord, Rect(113, b + 53, right - 113, 31), 19, TextColor, true);
            else {
                float x = 113;
                for (const auto& part : v.hero) {
                    const float width = Measure(part.value, 25, true, true);
                    Text(part.value, Rect(x, b + 52, width + 1, 33), 25, accent, true, true);
                    x += width + 2;
                    const float uw = Measure(part.unit, 12);
                    Text(part.unit, Rect(x, b + 62, uw + 1, 20), 12, Muted);
                    x += uw + 7;
                }
            }
            const float metricWidth = (right - 113 + 6) / 3;
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
            const float y = card.top;
            Text(T(Key::PowerChartPrefix) + v.powerLabel, Rect(25, y + 9, content * .60f, 18), 11.5f, Muted);
            Text(v.powerValue + T(Key::WattSuffix), Rect(25 + content * .60f, y + 9, content * .40f, 18), 12, TextColor, true, true, true);
            const auto chart = Rect(25, y + 34, content, 36);
            ChartGrid(chart);
            float maximum = 5;
            for (float value : v.powerHistory) maximum = std::max(maximum, value);
            Chart(chart, v.powerHistory.data(), v.powerHistory.size(),
                  v.mode == PowerMode::Discharging ? Drain : Ac, maximum * 1.15f);
            break;
        }
        case FullComponent::Network: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::Network), Rect(25, y + 9, networkLabelWidth_, 17), 11.5f, Muted);
            Text(v.totals, Rect(29 + networkLabelWidth_, y + 9, content - networkLabelWidth_ - 4, 17), 10.5f, Faint, false, true, true);
            auto speed = [&](const Speed& s, float x, const wchar_t* arrow, D2D1_COLOR_F color) {
                Text(arrow, Rect(x, y + 31, 12, 26), 13, color, true);
                const float width = std::min(74.0f, Measure(s.value, 19, true, true));
                Text(s.value, Rect(x + 13, y + 30, width + 1, 28), 19, TextColor, true, true);
                Text(s.unit, Rect(x + 17 + width, y + 38, 42, 18), 10.5f, Muted, false, true);
            };
            speed(v.up, 25, L"↑", Up);
            speed(v.down, 25 + half, L"↓", Down);
            Line(25, y + 64, right, y + 64, Border);
            const auto color = NetworkColor(v.netState);
            Text(T(Key::Latency), Rect(25, y + 77, latencyLabelWidth_, 24), 11.5f, Muted);
            Text(v.latency, Rect(34 + latencyLabelWidth_, y + 73, content - latencyLabelWidth_ - 9, 32), 23, color, true, true);
            Text(v.netHint, Rect(25, y + 110, content, 36), 11, color, false, false, false, true);
            break;
        }
        case FullComponent::Processes: {
            Panel(card);
            const float ptop = card.top;
            Text(T(Key::TopProcesses), Rect(25, ptop + 9, content * .55f, 17), 11.5f, Muted);
            Text(v.selfCpu, Rect(25 + content * .55f, ptop + 9, content * .45f, 17), 11, Faint, false, true, true);
            if (v.processes.empty()) Text(T(Key::Counting), Rect(25, ptop + 33, content, 19), 12, Faint);
            for (size_t i = 0; i < v.processes.size(); ++i) {
                const auto& p = v.processes[i];
                const float y = ptop + 33 + static_cast<float>(i) * 19;
                Text(p.name, Rect(25, y, content - 128, 19), 12, TextColor);
                Bar(Rect(right - 120, y + 7.5f, 70, 4), p.fraction, Color(0xf59e0b), Drain);
                Text(p.percent, Rect(right - 42, y, 42, 19), 12, Muted, false, true, true);
            }
            break;
        }
        case FullComponent::DiskIO: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::DiskIO), Rect(25, y + 9, content, 18), 11.5f, Muted);
            Text(T(Key::Read), Rect(25, y + 37, readLabelWidth_, 24), 11, Down);
            Text(v.read.value + L" " + v.read.unit, Rect(25 + readLabelWidth_, y + 35, half - readLabelWidth_ - 5, 28), 15, TextColor, true, true);
            Text(T(Key::Write), Rect(25 + half, y + 37, writeLabelWidth_, 24), 11, Up);
            Text(v.write.value + L" " + v.write.unit, Rect(25 + half + writeLabelWidth_, y + 35, content - half - writeLabelWidth_, 28), 15, TextColor, true, true);
            break;
        }
        case FullComponent::SystemDisk: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::SystemDrive), Rect(25, y + 9, content - 125, 18), 11.5f, Muted);
            Text(v.diskPercent, Rect(right - 117, y + 7, 117, 24), 17, v.disk >= .9f ? Danger : TextColor, true, true, true);
            Bar(Rect(25, y + 38, content, 5), v.disk, Down, Up);
            Text(T(Key::FreePrefix) + v.diskFree, Rect(25, y + 53, content, 18), 11.5f, Muted, false, true);
            break;
        }
        case FullComponent::SystemInfo: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::SystemInfo), Rect(25, y + 9, content, 18), 11.5f, Muted);
            auto row = [&](const std::wstring& label, const std::wstring& value, float top) {
                Text(label, Rect(25, y + top, systemLabelWidth_, 18), 11, Faint);
                Text(value, Rect(25 + systemLabelWidth_, y + top, content - systemLabelWidth_, 18), 12, TextColor, false, true, true);
            };
            row(T(Key::CpuFrequency), v.cpuFrequency, 33);
            row(T(Key::MemoryUsedTotal), v.memoryDetail, 51);
            row(T(Key::ProcessCount), v.processCount, 69);
            row(T(Key::Uptime), v.uptime, 87);
            break;
        }
        case FullComponent::GpuMemory: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::GpuMemory), Rect(25, y + 9, content, 18), 11.5f, Muted);
            const float column = v.gpuTempVisible ? content / 3 : half;
            const float secondWidth = v.gpuTempVisible ? column - 8 : content - half;
            Text(T(Key::SharedMemory), Rect(25, y + 33, column - 8, 17), 10.5f, Faint);
            Text(T(Key::DedicatedMemory), Rect(25 + column, y + 33, secondWidth, 17), 10.5f, Faint);
            Text(v.sharedMemory, Rect(25, y + 51, column - 8, 24), 16, Down, true, true);
            Text(v.dedicatedMemory, Rect(25 + column, y + 51, secondWidth, 24), 16, Up, true, true);
            if (v.gpuTempVisible) {
                Text(T(Key::Temperature), Rect(25 + 2 * column, y + 33, column, 17), 10.5f, Faint);
                Text(v.gpuTemp, Rect(25 + 2 * column, y + 51, column, 24), 16,
                    GpuTempColor(v, TextColor), true, true);
            }
            break;
        }
        case FullComponent::Wifi: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::Wifi), Rect(25, y + 9, 50, 18), 11.5f, Muted);
            Text(v.wifiSsid, Rect(80, y + 9, right - 80, 18), 12, TextColor, false, false, true);
            Text(T(Key::SignalStrength), Rect(25, y + 36, content - 100, 20), 11, Faint);
            Text(v.wifiSignal, Rect(right - 95, y + 34, 95, 24), 16, Charge, true, true, true);
            Bar(Rect(25, y + 63, content, 4), v.wifi, Down, Charge);
            break;
        }
        case FullComponent::BatteryHealth: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::BatteryHealth), Rect(25, y + 9, half - 8, 18), 11.5f, Muted);
            Text(T(Key::CycleCount), Rect(25 + half, y + 9, content - half, 18), 11.5f, Muted);
            Text(v.health, Rect(25, y + 34, half - 8, 30), 21, Charge, true, true);
            Text(v.cycles, Rect(25 + half, y + 34, content - half, 30), 21, TextColor, true, true);
            break;
        }
        case FullComponent::Fps: {
            Panel(card);
            const float y = card.top;
            Text(T(Key::Fps), Rect(25, y + 9, content, 18), 11.5f, Muted);
            const bool quiet = v.fpsAdmin || v.fps == L"--";
            Text(v.fps, Rect(25, y + 32, content, 28), v.fpsAdmin ? 13.0f : 16.0f, quiet ? Faint : Charge, !quiet);
            break;
        }
        default: break;
        }
    }
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->PopAxisAlignedClip();
    if (layout.height > viewport && viewport > 56) {
        const float track = viewport - 56;
        const float thumb = std::max(18.0f, track * (viewport - 44) / (layout.height - 44));
        const float top = 44 + (track - thumb) * scroll_ / (layout.height - viewport);
        Rounded(Rect(fullWidth_ - 7, top, 2, thumb), 1, Alpha(Muted, .45f));
    }
}

void Renderer::UpdateFullLayout() {
    // Text measurement happens on configuration/language changes, never in mini paints.
    networkLabelWidth_ = Measure(T(Key::Network), 11.5f) + 4;
    latencyLabelWidth_ = Measure(T(Key::Latency), 11.5f) + 4;
    readLabelWidth_ = Measure(T(Key::Read), 11) + 6;
    writeLabelWidth_ = Measure(T(Key::Write), 11) + 6;
    systemLabelWidth_ = 84;
    for (Key key : {Key::CpuFrequency, Key::MemoryUsedTotal, Key::ProcessCount, Key::Uptime})
        systemLabelWidth_ = std::max(systemLabelWidth_, Measure(T(key), 11) + 12);
    float chip = 0, scheme = Measure(T(Key::BatterySaver), 11);
    for (Key key : {Key::LowBattery, Key::Discharging, Key::Charging, Key::OnAc, Key::NoBattery, Key::Reading})
        chip = std::max(chip, Measure(T(key), 11) + 16);
    for (Key key : {Key::Balanced, Key::BestPerformance, Key::BestEfficiency, Key::LongerBattery})
        scheme = std::max(scheme, Measure(T(Key::PowerModePrefix) + T(key), 11));
    fullWidth_ = std::max(320.0f, 50 + chip + 8 + scheme);
    float hardware = 60;
    for (Key key : {Key::Cpu, Key::Gpu, Key::Memory, Key::Processor})
        hardware = std::max(hardware, Measure(T(key), 11) + 2);
    hardware = std::max(hardware, Measure(T(Key::WholeChip), 10.5f) + 2);
    fullWidth_ = std::max(fullWidth_, 50 + hardware * 4 + 30);
    fullWidth_ = std::max(fullWidth_, 50 + systemLabelWidth_ + Measure(L"999.9 / 999.9" + T(Key::GbSuffix), 12));
    for (Key key : {Key::FullyCharged, Key::AcPower, Key::NoBatteryDetected, Key::Calculating, Key::Reading})
        fullWidth_ = std::max(fullWidth_, 138 + Measure(T(key), 19, true));
    for (Key key : {Key::SinceLaunch, Key::SinceUnplugged})
        fullWidth_ = std::max(fullWidth_, 50 + Measure(T(key) + L"99+ " + T(Key::Hours) +
            T(Key::AveragePrefix) + L"999.9" + T(Key::WattSuffix), 11));
    fullWidth_ = std::ceil(fullWidth_);
}

void Renderer::UpdateCompactLayout() {
    // Reserve text widths once per configuration. Live digits must not resize
    // the HWND (and its D2D surface) or create measurement layouts every tick.
    compactCells_ = {};
    float width = 12;
    bool any = false;
    for (size_t i = 0; i < CompactComponents.size(); ++i) {
        if (!components_.compact[i]) continue;
        if (static_cast<CompactComponent>(i) == CompactComponent::Eta && !compactEtaVisible_) continue;
        auto& cell = compactCells_[i];
        cell.width = CompactComponents[i].extent;
        auto value = [&](const std::wstring& label, const std::wstring& sample) {
            cell.labelWidth = std::max(cell.labelWidth, Measure(label, 10.5f) + 4);
            cell.width = std::max(cell.width, cell.labelWidth + Measure(sample, 13, true, true) + 2);
        };
        switch (static_cast<CompactComponent>(i)) {
        case CompactComponent::Battery:
            cell.width = std::max(cell.width, 28 + Measure(L"100%", 13, true, true));
            break;
        case CompactComponent::Eta:
            value(T(Key::TimeLeft), L"99:59"); value(T(Key::FullIn), L"59" + T(Key::MinuteSuffix));
            value(T(Key::TimeLeft), L"99+" + T(Key::HourSuffix));
            cell.width = std::max(cell.width, std::max(Measure(T(Key::FullyCharged), 13, true, true),
                                                     Measure(T(Key::NotCharging), 13, true, true)) + 2);
            break;
        case CompactComponent::Power:
            value(T(Key::System), L"999.9" + T(Key::WattSuffix));
            value(T(Key::CompactProcessor), L"999.9" + T(Key::WattSuffix)); break;
        case CompactComponent::Cpu: value(T(Key::Cpu), L"100%"); break;
        case CompactComponent::Memory: value(T(Key::Memory), L"100%"); break;
        case CompactComponent::Gpu: value(T(Key::Gpu), L"100%"); break;
        case CompactComponent::GpuTemp: value(T(Key::TaskbarGpuTemp), L"150°C"); break;
        case CompactComponent::Network:
            cell.width = std::max(Measure(L"↑ 999.9" + T(Key::CompactM), 10.5f, false, true),
                                  Measure(L"↓ 999.9" + T(Key::CompactM), 10.5f, false, true)) + 2;
            break;
        case CompactComponent::DiskIO:
            cell.width = std::max(Measure(T(Key::ReadPrefix) + L"999.9" + T(Key::CompactM), 10.5f, false, true),
                                  Measure(T(Key::WritePrefix) + L"999.9" + T(Key::CompactM), 10.5f, false, true)) + 2;
            break;
        case CompactComponent::Latency:
            value(T(Key::Latency), L"9999" + T(Key::MsSuffix));
            value(T(Key::Latency), T(Key::Offline));
            value(T(Key::Latency), T(Key::Checking));
            break;
        case CompactComponent::CpuFrequency: value(T(Key::Frequency), L"99.99" + T(Key::GhzSuffix)); break;
        case CompactComponent::Fps:
            value(T(Key::FpsShort), L"9999");
            cell.width = std::max(cell.width, cell.labelWidth + Measure(T(Key::RunAsAdmin), 10.5f) + 2);
            break;
        default: break;
        }
        cell.width = std::ceil(cell.width);
        width += (any ? 9 : 0) + cell.width;
        any = true;
    }
    compactWidth_ = std::ceil(width + 9 + 24 + 6);
}
void Renderer::CompactContent(const View& v) {
    float x = 12;
    bool any = false;
    for (size_t i = 0; i < CompactComponents.size(); ++i) {
        if (!components_.compact[i]) continue;
        if (static_cast<CompactComponent>(i) == CompactComponent::Eta && !v.compactEtaVisible) continue;
        if (any) {
            Line(x + 4.5f, 12, x + 4.5f, 28, Border);
            x += 9;
        }
        any = true;
        const auto component = static_cast<CompactComponent>(i);
        const auto& cell = compactCells_[i];
        const float width = cell.width;
        auto value = [&](const std::wstring& label, const std::wstring& text, D2D1_COLOR_F color) {
            const float labelWidth = cell.labelWidth;
            Text(label, Rect(x, 11, labelWidth - 4, 18), 10.5f, Muted);
            Text(text, Rect(x + labelWidth, 8, width - labelWidth, 24), 13, color, true, true);
        };
        switch (component) {
        case CompactComponent::Battery: {
            const std::wstring text = v.percent + L"%";
            const bool charging = v.mode == PowerMode::Charging;
            const bool plugged = v.mode == PowerMode::AcIdle;
            const bool ac = charging || plugged;
            brush_->SetColor(ac ? Charge : Muted);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(Rect(x, 15, 18, 10), 2.5f, 2.5f), brush_.Get(), 1.5f);
            Rounded(Rect(x + 20, 18, 2, 4), 1, ac ? Charge : Muted);
            if (v.battery > 0)
                Rounded(Rect(x + 1.5f, 16.5f, 15 * v.battery, 7), 1,
                        ac ? Alpha(Charge, .20f) : BatteryColor(v));
            if (charging) Bolt(x + 4, 14, 12, Charge);
            else if (plugged) {
                Line(x + 6.5f, 17, x + 6.5f, 20, Charge, 1.2f);
                Line(x + 11.5f, 17, x + 11.5f, 20, Charge, 1.2f);
                Line(x + 6.5f, 20, x + 11.5f, 20, Charge, 1.5f);
                Line(x + 9, 20, x + 9, 23, Charge, 1.5f);
            }
            Text(text, Rect(x + 26, 8, width - 26, 24), 13, ac ? Charge : TextColor, true, true);
            break;
        }
        case CompactComponent::Eta: {
            auto color = v.mode == PowerMode::Discharging ? Drain :
                         v.mode == PowerMode::Charging ? Charge : TextColor;
            if (v.compactEtaFromSystem) color = Alpha(color, .65f);
            if (v.compactEtaLabel.empty())
                Text(v.compactEtaValue, Rect(x, 8, width, 24), 13, color, true, true);
            else value(v.compactEtaLabel, v.compactEtaValue, color);
            break;
        }
        case CompactComponent::Power: value(v.powerLabel, v.powerValue + T(Key::WattSuffix), Accent(v)); break;
        case CompactComponent::Cpu: value(T(Key::Cpu), v.cpuValue, Down); break;
        case CompactComponent::Memory: value(T(Key::Memory), v.memoryValue, Up); break;
        case CompactComponent::Gpu: value(T(Key::Gpu), v.gpuUsage, Charge); break;
        case CompactComponent::GpuTemp: value(T(Key::TaskbarGpuTemp), v.gpuTemp, GpuTempColor(v, Charge)); break;
        case CompactComponent::Network:
        case CompactComponent::DiskIO: {
            const bool network = component == CompactComponent::Network;
            const auto& first = network ? v.compactUp : v.diskRead;
            const auto& second = network ? v.compactDown : v.diskWrite;
            Text(first, Rect(x, 4, width, 16), 10.5f, network ? Up : Down, false, true);
            Text(second, Rect(x, 20, width, 16), 10.5f, network ? Down : Up, false, true);
            break;
        }
        case CompactComponent::Latency: value(T(Key::Latency), v.latency, NetworkColor(v.netState)); break;
        case CompactComponent::CpuFrequency: value(T(Key::Frequency), v.cpuFrequency, TextColor); break;
        case CompactComponent::Fps:
            if (v.fpsAdmin) {
                const float labelWidth = cell.labelWidth;
                Text(T(Key::FpsShort), Rect(x, 11, labelWidth - 4, 18), 10.5f, Muted);
                Text(v.fps, Rect(x + labelWidth, 11, width - labelWidth, 18), 10.5f, Faint);
            } else value(T(Key::FpsShort), v.fps, Charge);
            break;
        default: break;
        }
        x += width;
    }
}
void Renderer::Compact(const View& v, bool pinned, Button hover, Button pressed) {
    const float width = target_->GetSize().width;
    // A very large zoom can exceed a narrow monitor. Keep the mode button
    // visible and let the ordinary wheel scroll the clipped row horizontally.
    const bool clipped = compactWidth_ > width + .01f;
    if (clipped) {
        target_->PushAxisAlignedClip(Rect(0, 0, std::max(0.0f, width - 36), 40), D2D1_ANTIALIAS_MODE_ALIASED);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(-scroll_, 0));
    }
    CompactContent(v);
    if (clipped) {
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        target_->PopAxisAlignedClip();
    }
    Buttons(true, pinned, hover, pressed, width, Ac);
}
HRESULT Renderer::Paint(const View& v, bool compact, bool pinned, Button hover, Button pressed) {
    if (!CanRender()) return S_FALSE;
    // All target-dependent brushes are discarded together. Retry the entire frame once.
    for (int attempt = 0; attempt < 2; ++attempt) {
        HRESULT hr = CreateTarget();
        if (FAILED(hr)) return hr;
        if (!CanRender()) return S_FALSE;
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        const auto size = target_->GetSize();
        Background(size.width, size.height);
        if (compact) Compact(v, pinned, hover, pressed); else Full(v, pinned, hover, pressed);
        hr = Finish();
        if (hr != D2DERR_RECREATE_TARGET) return hr;
    }
    return D2DERR_RECREATE_TARGET;
}
float Renderer::ToastHeight(const std::wstring& message, float width) {
    auto* format = Format(12, false, false);
    ComPtr<IDWriteTextLayout> layout;
    if (!format || FAILED(write_->CreateTextLayout(message.c_str(), static_cast<UINT32>(message.size()),
                         format, std::max(1.0f, width - 50), 10000, layout.Put()))) return 112;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::ceil(std::max(96.0f, metrics.height + 65));
}
HRESULT Renderer::PaintToast(const NetEvent& event, const std::wstring& message) {
    if (!CanRender()) return S_FALSE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        HRESULT hr = CreateTarget();
        if (FAILED(hr)) return hr;
        if (!CanRender()) return S_FALSE;
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        const auto size = target_->GetSize();
        Background(size.width, size.height);
        const auto color = event.recovered ? Charge : Danger;
        Rounded(Rect(14, 16, 3, size.height - 32), 1.5f, color);
        Text(event.recovered ? T(Key::NetworkRestored) : T(Key::NetworkDisconnected), Rect(25, 12, size.width - 48, 24), 13.5f, color, true);
        Text(message, Rect(25, 41, size.width - 50, size.height - 65), 12, TextColor, false, false, false, true);
        Text(T(Key::AppName), Rect(25, size.height - 24, size.width - 50, 16), 10, Faint, false, false, true);
        hr = Finish();
        if (hr != D2DERR_RECREATE_TARGET) return hr;
    }
    return D2DERR_RECREATE_TARGET;
}
} // namespace pulse::ui
