#pragma once
#include "ui_renderer.h"
#include <memory>

namespace pulse::ui {
// The cross-process child only posts these messages. All work runs on the owner.
inline constexpr UINT TaskbarMessage = WM_APP + 7;
enum class TaskbarAction : WPARAM { Toggle = 1, Menu, Changed, Lost, Collapse, SettingsChanged };
enum class TaskbarPosition { Auto, Right, Left };
class TaskbarStrip {
public:
    TaskbarStrip();
    ~TaskbarStrip();
    TaskbarStrip(const TaskbarStrip&) = delete;
    TaskbarStrip& operator=(const TaskbarStrip&) = delete;
    bool Open(HINSTANCE instance, HWND owner);
    void Close(); // Restores the classic task-button area before destroying the child.
    void Configure(const Components& components, int zoom, TaskbarPosition position, int widgetsWidth);
    void SetTextColors(const TextColors& colors);
    COLORREF DefaultValueColor(CompactComponent component, const View& view) const;
    void Redraw(); // Appearance changes retain model, collection and text layouts.
    void ThemeChanged();
    void LanguageChanged();
    bool Update(const Snapshot& snapshot); // Tracks position and paints only when dirty.
    RECT ScreenRect() const;
    bool Visible() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pulse::ui
