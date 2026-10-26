#pragma once
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
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <map>
#include <array>
#include <cstdint>
#include <tuple>
#include "ui_model.h"

namespace pulse::ui {
template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& r) noexcept : value_(r.value_) { r.value_ = nullptr; }
    ComPtr& operator=(ComPtr&& r) noexcept {
        if (this != &r) { Reset(); value_ = r.value_; r.value_ = nullptr; }
        return *this;
    }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** Put() { Reset(); return &value_; }
    void Retain(T* value) { if (value) value->AddRef(); Reset(); value_ = value; }
    void Reset() { if (value_) { value_->Release(); value_ = nullptr; } }
private:
    T* value_ = nullptr;
};

enum class Button { None, Mode, Pin, Minimize, Close };
struct Layout {
    std::array<D2D1_RECT_F, FullComponents.size()> cards{};
    float height = 44;
    D2D1_RECT_F Card(FullComponent c) const { return cards[static_cast<size_t>(c)]; }
};
Layout FullLayout(const Components& components, float width = 320);

class Renderer {
public:
    HRESULT Initialize(bool fullPanel = true);
    void SetSoftware(bool software);
    void SetTaskbarDetails(bool details) { taskbarDetails_ = details; }
    void SetWindow(HWND window, UINT dpi);
    void SetDpi(UINT dpi);
    void SetZoom(int percent);
    void LanguageChanged();
    void Resize();
    void SetComponents(const Components& components, bool compact);
    bool SetCompactEtaVisible(bool visible);
    void SetScroll(float offset) { scroll_ = offset; }
    void DiscardTarget();
    bool CanRender() const;
    float CompactWidth() const { return compactWidth_; }
    float FullWidth() const { return fullWidth_; }
    float ToastHeight(const std::wstring& message, float width = 300);
    Button HitTest(float x, float y, bool compact, float width) const;
    D2D1_RECT_F ButtonRect(Button button, bool compact, float width) const;
    HRESULT Paint(const View& v, bool compact, bool pinned, Button hover, Button pressed);
    HRESULT PaintToast(const NetEvent& event, const std::wstring& message);
private:
    using FormatKey = std::tuple<int, int, bool>;
    HRESULT CreateTarget();
    HRESULT Finish();
    float RenderDpi() const { return static_cast<float>(dpi_) * zoom_ / 100.0f; }
    IDWriteTextFormat* Format(float size, bool bold, bool numeric);
    IDWriteTextLayout* TextLayout(const std::wstring& text, float size, bool bold, bool numeric,
                                 float width, float height, bool right, bool wrap);
    ID2D1LinearGradientBrush* LinearGradient(D2D1_COLOR_F from, D2D1_COLOR_F to);
    float Measure(const std::wstring& text, float size, bool bold = false, bool numeric = false);
    void Text(const std::wstring& text, D2D1_RECT_F rect, float size, D2D1_COLOR_F color,
              bool bold = false, bool numeric = false, bool right = false, bool wrap = false);
    void Background(float width, float height);
    void Panel(D2D1_RECT_F rect);
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width = 1);
    void Rounded(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color);
    void Gradient(D2D1_RECT_F rect, D2D1_COLOR_F from, D2D1_COLOR_F to, bool vertical, float radius = 0);
    void Bar(D2D1_RECT_F rect, float fraction, D2D1_COLOR_F from, D2D1_COLOR_F to);
    void Ring(const View& v, float x, float y);
    void Bolt(float x, float y, float size, D2D1_COLOR_F color);
    void Buttons(bool compact, bool pinned, Button hover, Button pressed, float width, D2D1_COLOR_F accent);
    void Full(const View& v, bool pinned, Button hover, Button pressed);
    void Compact(const View& v, bool pinned, Button hover, Button pressed);
    void Chart(D2D1_RECT_F rect, const float* a, size_t count, D2D1_COLOR_F color, float maximum);
    void ChartGrid(D2D1_RECT_F rect);
    void UpdateCompactLayout();
    void UpdateFullLayout();
    void CompactContent(const View& v);
    void ValueUnit(const std::wstring& value, const std::wstring& unit, float x, float y,
                   float numberSize, D2D1_COLOR_F color);

    Components components_;
    struct CompactCell { float width = 0, labelWidth = 0; };
    std::array<CompactCell, CompactComponents.size()> compactCells_{};
    float compactWidth_ = 51, fullWidth_ = 320;
    float fullSchemeWidth_ = 0;
    float networkLabelWidth_ = 32, latencyLabelWidth_ = 34, systemLabelWidth_ = 84;
    float readLabelWidth_ = 20, writeLabelWidth_ = 20;
    bool chartVisible_ = false, compact_ = false, fullPanel_ = true, software_ = false;
    bool compactEtaVisible_ = true, taskbarDetails_ = false;
    float scroll_ = 0;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    int zoom_ = 100;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<IDWriteTypography> tabular_;
    ComPtr<IDWriteInlineObject> ellipsis_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    ComPtr<ID2D1PathGeometry> bolt_;
    std::map<FormatKey, ComPtr<IDWriteTextFormat>> formats_;
    using LayoutKey = std::tuple<std::wstring, FormatKey, float, float, bool, bool>;
    struct CachedLayout {
        ComPtr<IDWriteTextLayout> layout;
        std::uint64_t used = 0;
    };
    std::map<LayoutKey, CachedLayout> layouts_; // Bounded LRU, including measurement layouts.
    std::uint64_t layoutSerial_ = 0;
    std::map<std::array<float, 8>, ComPtr<ID2D1LinearGradientBrush>> gradients_;
    ComPtr<ID2D1RadialGradientBrush> downGlow_, upGlow_;
    std::wstring font_, numericFont_;
};

void StyleWindow(HWND window);
UINT WindowDpi(HWND window);
UINT MonitorDpi(HMONITOR monitor);
} // namespace pulse::ui
