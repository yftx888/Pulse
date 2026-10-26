#include "ui_taskbar.h"
#include "netprobe.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>
#include <vector>

namespace pulse::ui {
namespace {
constexpr wchar_t StripClass[] = L"Pulse.Native.Taskbar.v1";
constexpr float LabelGap = 2;
struct TaskbarDpiContext {
    using SetFn = HANDLE(WINAPI*)(HANDLE);
    SetFn set = nullptr;
    HANDLE previous = nullptr;
    explicit TaskbarDpiContext(HWND shell) {
        const HMODULE user = GetModuleHandleW(L"user32.dll");
        using GetFn = HANDLE(WINAPI*)(HWND);
        const auto get = reinterpret_cast<GetFn>(GetProcAddress(user, "GetWindowDpiAwarenessContext"));
        set = reinterpret_cast<SetFn>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
        if (get && set) previous = set(get(shell));
    }
    ~TaskbarDpiContext() { if (set && previous) set(previous); }
};
constexpr wchar_t AdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
DWORD RegistryNumber(const wchar_t* path, const wchar_t* name, DWORD fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return fallback;
    DWORD result = fallback, size = sizeof(result), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&result), &size) != ERROR_SUCCESS ||
        type != REG_DWORD) result = fallback;
    RegCloseKey(key);
    return result;
}
bool SameRect(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
RECT ClientPosition(HWND window, HWND parent) {
    RECT r{};
    GetWindowRect(window, &r);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
bool Windows11() {
    using VersionFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    const auto version = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers"));
    DWORD major = 0, minor = 0, build = 0;
    if (version) version(&major, &minor, &build);
    return major >= 10 && (build & 0x0fffffff) >= 22000;
}
Key Label(CompactComponent c, PowerMode mode = PowerMode::Unknown) {
    switch (c) {
    case CompactComponent::Battery: return Key::TaskbarBattery;
    case CompactComponent::Eta: return mode == PowerMode::Charging ? Key::TaskbarFull : Key::TaskbarEta;
    case CompactComponent::Power: return Key::TaskbarPower;
    case CompactComponent::Cpu: return Key::TaskbarCpu;
    case CompactComponent::Memory: return Key::TaskbarMemory;
    case CompactComponent::Gpu: return Key::TaskbarGpu;
    case CompactComponent::GpuTemp: return Key::TaskbarGpuTemp;
    case CompactComponent::Latency: return Key::TaskbarPing;
    case CompactComponent::CpuFrequency: return Key::TaskbarFrequency;
    default: return Key::TaskbarFps;
    }
}
} // namespace

struct TaskbarStrip::Impl {
    HWND window = nullptr, owner = nullptr, shell = nullptr, bar = nullptr, tasks = nullptr;
    HINSTANCE instance = nullptr;
    Components components;
    Model model;
    bool dirty = true, layoutDirty = true, light = false, classic = false, closing = false;
    bool widgets = true, centered = true, reserved = false, registered = false, resizePending = false;
    bool reserveAgain = false, themeRead = false;
    TaskbarPosition position = TaskbarPosition::Auto;
    // Auto is the uninitialized state; afterwards remember Left or Right.
    TaskbarPosition autoSide = TaskbarPosition::Auto;
    int widgetsWidth = 120;
    std::vector<DWORD> loggedErrors;
    HWND restoredTasks = nullptr;
    RECT restoredOriginal{}, restoredFrom{};
    UINT dpi = 96;
    int zoom = 100, width = 1, height = 1, layoutHeight = 0, bitmapWidth = 0, bitmapHeight = 0;
    float scale = 1, fontSize = 10.0f, logicalWidth = 1;
    RECT originalTasks{}, shrunkTasks{}, requestedFrom{}, previousBar{}, placed{};
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteTypography> tabular;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    struct Cell { CompactComponent component; int row; float x, width, label; };
    std::vector<Cell> cells;
    struct TextCache { std::wstring text; float width = 0, height = 0; bool right = false; ComPtr<IDWriteTextLayout> layout; };
    std::vector<TextCache> texts;

    bool Failure(const wchar_t* step, DWORD error) {
        // Called only on the owner thread, never from the cross-process child.
        // Remember all codes in this outage, including across Close/Open retries.
        if (std::find(loggedErrors.begin(), loggedErrors.end(), error) == loggedErrors.end()) {
            loggedErrors.push_back(error);
            diagnostics::WriteLog(L"UI", std::wstring(L"Taskbar attach failed: step=") + step +
                L" err=" + std::to_wstring(error));
        }
        return false;
    }

    ~Impl() { Close(); if (registered) UnregisterClassW(StripClass, instance); }
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, msg, w, l);
        // Never collect, paint, read settings, or write files here: Explorer can
        // synchronously wait for this child during layout/activation messages.
        TaskbarAction action = TaskbarAction::Changed;
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
        case WM_LBUTTONUP: action = TaskbarAction::Toggle; break;
        case WM_CONTEXTMENU: action = TaskbarAction::Menu; break;
        case WM_SIZE:
            self->placed = {}; self->dirty = true; break;
        case WM_DPICHANGED: case WM_DISPLAYCHANGE:
            self->dirty = true; self->layoutDirty = true; break;
        case WM_SETTINGCHANGE:
            self->dirty = true; self->layoutDirty = true;
            action = TaskbarAction::SettingsChanged; break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->window = nullptr;
            action = TaskbarAction::Lost;
            break;
        default: return DefWindowProcW(hwnd, msg, w, l);
        }
        if (!self->closing) PostMessageW(self->owner, TaskbarMessage, static_cast<WPARAM>(action), 0);
        return msg == WM_NCDESTROY ? DefWindowProcW(hwnd, msg, w, l) : 0;
    }
    void ReadTheme() {
        themeRead = true;
        const bool value = RegistryNumber(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", 0) != 0;
        if (value != light) { light = value; dirty = true; }
        widgets = RegistryNumber(AdvancedKey, L"TaskbarDa", 1) != 0;
        centered = RegistryNumber(AdvancedKey, L"TaskbarAl", 1) != 0;
    }
    bool Initialize() {
        if (factory && write && tabular) return true;
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.Put());
        if (FAILED(hr)) return Failure(L"D2D1CreateFactory", static_cast<DWORD>(hr));
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write.Put()));
        if (FAILED(hr)) return Failure(L"DWriteCreateFactory", static_cast<DWORD>(hr));
        hr = write->CreateTypography(tabular.Put());
        if (FAILED(hr)) return Failure(L"CreateTypography", static_cast<DWORD>(hr));
        hr = tabular->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
        if (FAILED(hr)) { tabular.Reset(); return Failure(L"AddFontFeature", static_cast<DWORD>(hr)); }
        return true;
    }
    void ReleaseSurface() {
        brush.Reset(); target.Reset();
        if (memory) {
            if (oldBitmap) SelectObject(memory, oldBitmap);
            if (bitmap) DeleteObject(bitmap);
            DeleteDC(memory);
        }
        memory = nullptr; bitmap = nullptr; oldBitmap = nullptr;
        bitmapWidth = bitmapHeight = 0;
    }
    void RestoreTasks() {
        if (reserved && IsWindow(tasks) && IsWindow(bar)) {
            const RECT current = ClientPosition(tasks, bar);
            // If Explorer already supplied a new natural layout, do not overwrite it.
            if (resizePending || SameRect(current, shrunkTasks)) {
                restoredTasks = tasks; restoredOriginal = originalTasks; restoredFrom = current;
                SetWindowPos(tasks, nullptr, originalTasks.left, originalTasks.top,
                    originalTasks.right - originalTasks.left, originalTasks.bottom - originalTasks.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            }
        }
        reserved = resizePending = reserveAgain = false;
    }
    void Close() {
        closing = true;
        RestoreTasks();
        if (window && IsWindow(window)) DestroyWindow(window);
        window = nullptr; shell = bar = tasks = nullptr;
        ReleaseSurface(); placed = {};
        closing = false;
        dirty = layoutDirty = true;
    }
    bool Attach() {
        SetLastError(ERROR_SUCCESS);
        shell = FindWindowW(L"Shell_TrayWnd", nullptr);
        RECT r{};
        if (!shell) {
            const DWORD error = GetLastError();
            return Failure(L"FindWindow(Shell_TrayWnd)", error ? error : ERROR_FILE_NOT_FOUND);
        }
        if (!GetWindowRect(shell, &r)) return Failure(L"GetWindowRect(Shell_TrayWnd)", GetLastError());
        if (r.right - r.left <= r.bottom - r.top) return Failure(L"TaskbarOrientation", ERROR_NOT_SUPPORTED);
        bar = FindWindowExW(shell, nullptr, L"ReBarWindow32", nullptr);
        tasks = bar ? FindWindowExW(bar, nullptr, L"MSTaskSwWClass", nullptr) : nullptr;
        if (!tasks && bar) tasks = FindWindowExW(bar, nullptr, L"MSTaskListWClass", nullptr);
        classic = !Windows11();
        if (classic && !bar) {
            bar = FindWindowExW(shell, nullptr, L"WorkerW", nullptr);
            tasks = bar ? FindWindowExW(bar, nullptr, L"MSTaskSwWClass", nullptr) : nullptr;
            if (!tasks && bar) tasks = FindWindowExW(bar, nullptr, L"MSTaskListWClass", nullptr);
        }
        if (classic && !tasks) return Failure(L"FindWindow(TaskButtons)", ERROR_FILE_NOT_FOUND);
        if (!registered) {
            WNDCLASSEXW cls{};
            cls.cbSize = sizeof(cls); cls.lpfnWndProc = Procedure;
            cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_HAND);
            cls.lpszClassName = StripClass;
            if (!RegisterClassExW(&cls)) return Failure(L"RegisterClassEx", GetLastError());
            registered = true;
        }
        // Match Explorer while creating/reparenting the child, then restore the
        // UI thread context so the floating panel retains its own DPI behavior.
        TaskbarDpiContext context(shell);
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
            StripClass, T(Key::AppName).c_str(), WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
        if (!window) return Failure(L"CreateWindowEx", GetLastError());
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR oldStyle = SetWindowLongPtrW(window, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
        const DWORD styleError = GetLastError();
        if (!oldStyle && styleError != ERROR_SUCCESS) { Close(); return Failure(L"SetWindowLongPtr(Style)", styleError); }
        SetLastError(ERROR_SUCCESS);
        const HWND old = SetParent(window, shell);
        const DWORD parentError = GetLastError();
        if (!old && parentError != ERROR_SUCCESS) { Close(); return Failure(L"SetParent", parentError); }
        SetLastError(ERROR_SUCCESS);
        if (GetParent(window) != shell) {
            const DWORD error = GetLastError();
            Close();
            return Failure(L"GetParent", error ? error : ERROR_INVALID_WINDOW_HANDLE);
        }
        dpi = WindowDpi(shell);
        if (classic) {
            originalTasks = ClientPosition(tasks, bar);
            // A quick mode switch can re-enter before Explorer has handled the
            // queued restoration. Reuse its original width and queue our new
            // reservation after that restoration, even if the current size fits.
            if (tasks == restoredTasks && SameRect(originalTasks, restoredFrom)) {
                originalTasks = restoredOriginal;
                shrunkTasks = restoredFrom;
                reserved = reserveAgain = true;
            }
            restoredTasks = nullptr;
            GetClientRect(bar, &previousBar);
        }
        placed = {}; dirty = layoutDirty = true;
        return true;
    }
    float Measure(const std::wstring& value) {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(write->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), format.Get(),
            10000, 100, layout.Put()))) return 0;
        layout->SetTypography(tabular.Get(), {0, static_cast<UINT32>(value.size())});
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.widthIncludingTrailingWhitespace;
    }
    float ValueWidth(CompactComponent c) {
        float result = Measure(L"--");
        auto fit = [&](const std::wstring& value) { result = std::max(result, Measure(value)); };
        switch (c) {
        case CompactComponent::Network: case CompactComponent::DiskIO:
            // FormatSpeed: 0 decimals at >=100, 1 at >=10, otherwise 2
            // for MB/GB and 1 for KB. Include rounding carries (99.95 -> 100.0,
            // 999.5 -> 1000), rather than impossible five-digit decimals.
            for (auto unit : {Key::Kbps, Key::Mbps, Key::Gbps}) {
                fit(L"100.0 " + T(unit)); fit(L"1000 " + T(unit));
            }
            fit(L"9.99 " + T(Key::Mbps)); fit(L"9.99 " + T(Key::Gbps)); break;
        case CompactComponent::Power:
            // Number(watts, watts >= 100 ? 0 : 1), including the carry.
            fit(L"100.0" + T(Key::WattSuffix)); fit(L"9999" + T(Key::WattSuffix)); break;
        case CompactComponent::Latency:
            // >=1000 ms is classified as unavailable by the current probe.
            fit(L"999" + T(Key::MsSuffix)); fit(T(Key::UnknownLatency));
            fit(T(Key::Checking)); fit(T(Key::Offline)); break;
        case CompactComponent::Eta:
            fit(L"88:88"); fit(L"59" + T(Key::MinuteSuffix)); fit(L"99+" + T(Key::HourSuffix));
            fit(T(Key::AssistChip)); break;
        case CompactComponent::CpuFrequency: fit(L"10.00" + T(Key::GhzSuffix)); break;
        case CompactComponent::Fps: fit(L"8888"); fit(T(Key::RunAsAdmin)); break;
        case CompactComponent::GpuTemp: fit(L"150°C"); break;
        default: fit(L"100%"); break;
        }
        return std::ceil(result) + 1;
    }
    float LabelWidth(CompactComponent c) {
        float result = Measure(T(Label(c)));
        if (c == CompactComponent::Eta) result = std::max(result, Measure(T(Key::TaskbarFull)));
        if (c == CompactComponent::Power) {
            for (Key key : {Key::System, Key::CompactProcessor, Key::Battery, Key::Power})
                result = std::max(result, Measure(T(key)));
        }
        return std::ceil(result) + LabelGap;
    }
    float CellWidth(CompactComponent c) {
        float result = LabelWidth(c) + ValueWidth(c);
        if (c == CompactComponent::Eta) {
            // Like compact mode, idle status text uses the whole cell without a
            // label. Reserve it now so AC/charge/discharge never resize the strip.
            result = std::max(result, std::ceil(std::max(Measure(T(Key::FullyCharged)),
                std::max(Measure(T(Key::NotCharging)), Measure(T(Key::AssistChip))))) + 1);
        }
        return result;
    }
    bool Layout(int taskbarHeight) {
        // Physical taskbar height already includes monitor DPI. Fit two 18-DIP
        // rows to its logical height, with zoom capped at the available pixels.
        const float heightDips = taskbarHeight * 96.0f / dpi;
        scale = std::min(dpi / 96.0f * zoom / 100.0f * heightDips / 48.0f, taskbarHeight / 36.0f);
        scale = std::max(.1f, scale);
        fontSize = 10.0f;
        format.Reset();
        const HRESULT hr = write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize,
            i18n::LocaleName(), format.Put());
        if (FAILED(hr)) return Failure(L"CreateTextFormat", static_cast<DWORD>(hr));
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        cells.clear(); texts.clear();
        constexpr float padding = 2, columnGap = 4;
        float x = padding;
        // Speed pairs own a column. ETA/power share a column when both are on;
        // all remaining cells keep their enum order and pair without gaps.
        std::vector<CompactComponent> ordinary;
        auto addPair = [&](CompactComponent c) {
            if (!components.taskbar[static_cast<size_t>(c)]) return;
            const float label = std::ceil(c == CompactComponent::Network ?
                std::max(Measure(L"↑"), Measure(L"↓")) :
                std::max(Measure(T(Key::TaskbarRead)), Measure(T(Key::TaskbarWrite)))) + LabelGap;
            const float width = label + ValueWidth(c);
            cells.push_back({c, 0, x, width, label}); cells.push_back({c, 1, x, width, label});
            x += width + columnGap;
        };
        addPair(CompactComponent::Network);
        const bool batteryPair = components.taskbar[static_cast<size_t>(CompactComponent::Eta)] &&
            components.taskbar[static_cast<size_t>(CompactComponent::Power)];
        if (batteryPair) {
            const float width = std::max(CellWidth(CompactComponent::Eta), CellWidth(CompactComponent::Power));
            cells.push_back({CompactComponent::Eta, 0, x, width, LabelWidth(CompactComponent::Eta)});
            cells.push_back({CompactComponent::Power, 1, x, width, LabelWidth(CompactComponent::Power)});
            x += width + columnGap;
        }
        for (size_t i = 0; i < components.taskbar.size(); ++i) {
            const auto c = static_cast<CompactComponent>(i);
            if (!components.taskbar[i] || c == CompactComponent::Network || c == CompactComponent::DiskIO) continue;
            if (batteryPair && (c == CompactComponent::Eta || c == CompactComponent::Power)) continue;
            ordinary.push_back(c);
        }
        for (size_t i = 0; i < ordinary.size(); i += 2) {
            float width = 0;
            for (size_t j = i; j < std::min(i + 2, ordinary.size()); ++j)
                width = std::max(width, CellWidth(ordinary[j]));
            for (size_t j = i; j < std::min(i + 2, ordinary.size()); ++j)
                cells.push_back({ordinary[j], static_cast<int>(j - i), x, width,
                    LabelWidth(ordinary[j])});
            x += width + columnGap;
        }
        addPair(CompactComponent::DiskIO);
        logicalWidth = std::max(24.0f, x - columnGap + padding);
        width = static_cast<int>(std::ceil(logicalWidth * scale));
        height = std::min(taskbarHeight, std::max(1, static_cast<int>(std::ceil(36 * scale))));
        texts.resize(cells.size() * 2);
        layoutHeight = taskbarHeight;
        layoutDirty = false; dirty = true;
        return true;
    }
    bool Track() {
        RECT client{};
        if (!GetClientRect(shell, &client)) return Failure(L"GetClientRect(Shell_TrayWnd)", GetLastError());
        if (client.right <= client.bottom || client.bottom <= 0) return Failure(L"TaskbarClientSize", ERROR_NOT_SUPPORTED);
        const UINT nextDpi = WindowDpi(shell);
        if (nextDpi != dpi) { dpi = nextDpi; layoutDirty = true; }
        if (layoutHeight != client.bottom) layoutDirty = true;
        if (layoutDirty && !Layout(static_cast<int>(client.bottom))) return false;
        const int clientWidth = static_cast<int>(client.right);
        if (width > clientWidth) return Failure(L"TaskbarSpace", ERROR_INSUFFICIENT_BUFFER);
        int x = 0;
        if (classic) {
            if (!IsWindow(tasks) || !IsWindow(bar)) return Failure(L"TaskButtonsWindow", ERROR_INVALID_WINDOW_HANDLE);
            RECT current = ClientPosition(tasks, bar), barClient{};
            GetClientRect(bar, &barClient);
            if (SameRect(current, shrunkTasks)) resizePending = false;
            if (reserved && (SameRect(current, shrunkTasks) ||
                (resizePending && SameRect(current, requestedFrom)))) {
                originalTasks.right += barClient.right - previousBar.right;
            } else { originalTasks = current; resizePending = false; }
            previousBar = barClient;
            RECT desired = originalTasks;
            desired.right -= width + 2;
            if (desired.right <= desired.left) return Failure(L"TaskButtonsSpace", ERROR_INSUFFICIENT_BUFFER);
            if (reserveAgain || (!SameRect(current, desired) && !(resizePending && SameRect(desired, shrunkTasks)))) {
                // Cross-process geometry is asynchronous: a busy Explorer must
                // never stall the sampling/UI thread waiting for WM_WINDOWPOS*.
                if (!SetWindowPos(tasks, nullptr, desired.left, desired.top,
                    desired.right - desired.left, desired.bottom - desired.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) return Failure(L"SetWindowPos(TaskButtons)", GetLastError());
                requestedFrom = current; resizePending = true; reserveAgain = false;
            }
            shrunkTasks = desired; reserved = true;
            POINT p{desired.right + 2, desired.top};
            MapWindowPoints(bar, shell, &p, 1); x = p.x;
        } else {
            const HWND notify = FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr);
            RECT notification{};
            int right = static_cast<int>(client.right) - static_cast<int>(std::ceil(160 * dpi / 96.0f));
            if (notify && GetWindowRect(notify, &notification)) {
                MapWindowPoints(nullptr, shell, reinterpret_cast<POINT*>(&notification), 2);
                if (notification.left > 0 && notification.right > notification.left) right = notification.left;
            }
            // Left-aligned Win11 can put Widgets next to the notification area.
            if (widgets && !centered) right -= static_cast<int>(std::ceil(160 * dpi / 96.0f));
            const int rightX = right - width - 2;
            x = rightX;
            // A fixed Left position follows Start regardless of taskbar alignment.
            if (centered || position == TaskbarPosition::Left) {
                const HWND start = FindWindowExW(shell, nullptr, L"Start", nullptr);
                RECT button{};
                if (start && GetWindowRect(start, &button)) {
                    // Use taskbar-client coordinates even on a monitor with a
                    // nonzero/negative desktop origin. Win11's task-list/rebar
                    // rectangles do not describe the actual centered icons.
                    MapWindowPoints(nullptr, shell, reinterpret_cast<POINT*>(&button), 2);
                    const int startLeft = static_cast<int>(button.left);
                    if (startLeft >= 0 && button.right > button.left && button.right <= clientWidth) {
                        const int gap = static_cast<int>(std::ceil(4 * dpi / 96.0f));
                        const int iconsRight = clientWidth - startLeft;
                        const int leftX = startLeft - gap - width;
                        const int leftLimit = widgets ? static_cast<int>(std::ceil(widgetsWidth * dpi / 96.0f)) : 0;
                        const bool rightFits = rightX >= iconsRight + gap && rightX + width <= clientWidth;
                        const bool leftFits = leftX >= leftLimit && leftX + width <= clientWidth;
                        if (position == TaskbarPosition::Left) {
                            // Widgets reserve is only an estimate, not a limit
                            // for a fixed side or Auto's no-space fallback.
                            x = leftX;
                        } else if (position == TaskbarPosition::Auto) {
                            const auto previousSide = autoSide;
                            if (autoSide == TaskbarPosition::Auto) {
                                autoSide = !rightFits && leftFits ? TaskbarPosition::Left : TaskbarPosition::Right;
                            } else if (autoSide == TaskbarPosition::Left) {
                                if (!leftFits && rightFits) autoSide = TaskbarPosition::Right;
                            } else if (!rightFits && leftFits) autoSide = TaskbarPosition::Left;
                            // Switch only when the other side fits and this one
                            // does not. If neither fits, keep the current side.
                            x = autoSide == TaskbarPosition::Left ? leftX : rightX;
                            if (autoSide != previousSide) dirty = true;
                        }
                    }
                }
                // If Start is unavailable, x stays beside the notification area.
            }
        }
        x = std::clamp(x, 0, clientWidth - width);
        const int y = (static_cast<int>(client.bottom) - height) / 2;
        const RECT next{x, y, x + width, y + height};
        if (!SameRect(next, placed)) {
            if (!MoveWindow(window, x, y, width, height, FALSE)) return Failure(L"MoveWindow", GetLastError());
            placed = next;
            dirty = true; // A side change needs a complete layered-window frame.
        }
        return true;
    }
    bool Surface() {
        if (bitmapWidth == width && bitmapHeight == height && target) return true;
        ReleaseSurface();
        memory = CreateCompatibleDC(nullptr);
        if (!memory) return Failure(L"CreateCompatibleDC", GetLastError());
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) { const DWORD error = GetLastError(); ReleaseSurface(); return Failure(L"CreateDIBSection", error); }
        oldBitmap = SelectObject(memory, bitmap);
        if (!oldBitmap || oldBitmap == HGDI_ERROR) {
            const DWORD error = GetLastError();
            oldBitmap = nullptr; ReleaseSurface(); return Failure(L"SelectObject", error);
        }
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        const RECT rect{0, 0, width, height};
        HRESULT hr = factory->CreateDCRenderTarget(&properties, target.Put());
        const wchar_t* step = L"CreateDCRenderTarget";
        if (SUCCEEDED(hr)) { step = L"BindDC"; hr = target->BindDC(memory, &rect); }
        if (SUCCEEDED(hr)) { step = L"CreateSolidColorBrush"; hr = target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), brush.Put()); }
        if (FAILED(hr)) { ReleaseSurface(); return Failure(step, static_cast<DWORD>(hr)); }
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        bitmapWidth = width; bitmapHeight = height;
        return true;
    }
    void DrawText(size_t index, const std::wstring& value, D2D1_RECT_F rect, bool right, D2D1_COLOR_F color) {
        auto& cache = texts[index];
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        if (!cache.layout || cache.text != value || cache.width != w || cache.height != h || cache.right != right) {
            cache.layout.Reset(); cache.text = value; cache.width = w; cache.height = h; cache.right = right;
            if (FAILED(write->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), format.Get(),
                w, h, cache.layout.Put()))) return;
            cache.layout->SetTextAlignment(right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            cache.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            cache.layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            cache.layout->SetTypography(tabular.Get(), {0, static_cast<UINT32>(value.size())});
        }
        brush->SetColor(color);
        target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), cache.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    bool Paint() {
        if (!Surface()) return false;
        const auto& v = model.Data();
        auto color = [&](unsigned dark, unsigned pale) { return D2D1::ColorF(light ? pale : dark); };
        const auto normal = color(0xe9edf6, 0x202634), muted = color(0xb0bbd0, 0x41495b);
        const auto up = color(0xa78bfa, 0x6942b3), down = color(0x38bdf8, 0x006c9d);
        const auto green = color(0x34d399, 0x00774b), yellow = color(0xfbbf24, 0x865b00), red = color(0xf87171, 0xb51c27);
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        // Alpha 1/255 makes the whole strip hit-testable. Nonzero RGB also avoids
        // Explorer's pure-black layered-child context-menu issue on Win11.
        target->Clear(D2D1::ColorF(0xffffff, 1.0f / 255)); // Premultiplied DIB bytes: BGRA = 1,1,1,1.
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto& cell = cells[i];
            const auto c = cell.component;
            std::wstring label, value;
            auto ink = normal;
            if (c == CompactComponent::Network || c == CompactComponent::DiskIO) {
                const bool net = c == CompactComponent::Network;
                label = net ? (cell.row ? L"↓" : L"↑") : T(cell.row ? Key::TaskbarWrite : Key::TaskbarRead);
                value = net ? (cell.row ? v.compactDown : v.compactUp) : (cell.row ? v.diskWrite : v.diskRead);
                ink = cell.row == (net ? 1 : 0) ? down : up;
            } else {
                label = T(Label(c, v.mode));
                switch (c) {
                case CompactComponent::Battery:
                    value = v.percent + L"%";
                    ink = v.supplemental ? yellow : v.mode == PowerMode::Charging || (v.mode == PowerMode::AcIdle && v.acOnline) ? green :
                        v.battery >= 0 && v.battery <= .2f ? red : normal; break;
                case CompactComponent::Eta:
                    value = !v.compactEtaVisible || v.compactEtaValue.empty() ? L"--" : v.compactEtaValue;
                    if (v.compactEtaVisible && v.compactEtaLabel.empty()) label.clear();
                    ink = v.supplemental ? yellow : v.mode == PowerMode::Charging ? green : normal;
                    break;
                case CompactComponent::Power:
                    if (!v.powerLabel.empty()) label = v.powerLabel;
                    value = v.powerValue + T(Key::WattSuffix);
                    ink = v.low ? red : v.mode == PowerMode::Charging ? green : yellow; break;
                case CompactComponent::Cpu: value = v.cpuValue; ink = down; break;
                case CompactComponent::Memory: value = v.memoryValue; ink = up; break;
                case CompactComponent::Gpu: value = v.gpuUsage; ink = green; break;
                case CompactComponent::GpuTemp:
                    value = v.gpuTemp; ink = v.gpuTempLevel >= 2 ? red : v.gpuTempLevel == 1 ? yellow : green; break;
                case CompactComponent::Latency:
                    value = v.latency; ink = v.netState == NetState::Offline ? red : v.netState == NetState::Slow ? yellow :
                        v.netState == NetState::Checking ? muted : green; break;
                case CompactComponent::CpuFrequency: value = v.cpuFrequency; break;
                case CompactComponent::Fps: value = v.fps; ink = v.fpsAdmin ? muted : green; break;
                default: break;
                }
            }
            const float y = 18.0f * cell.row;
            if (!label.empty())
                DrawText(2 * i, label, D2D1::RectF(cell.x, y, cell.x + cell.label - LabelGap, y + 18), false, muted);
            DrawText(2 * i + 1, value, D2D1::RectF(cell.x + (label.empty() ? 0 : cell.label), y,
                cell.x + cell.width, y + 18), true, ink);
        }
        const HRESULT hr = target->EndDraw();
        if (FAILED(hr)) { ReleaseSurface(); return Failure(L"EndDraw", static_cast<DWORD>(hr)); }
        POINT source{0, 0};
        SIZE size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window, nullptr, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA))
            return Failure(L"UpdateLayeredWindow", GetLastError());
        ShowWindow(window, SW_SHOWNOACTIVATE);
        dirty = false;
        return true;
    }
};
TaskbarStrip::TaskbarStrip() : impl_(std::make_unique<Impl>()) {}
TaskbarStrip::~TaskbarStrip() = default;
bool TaskbarStrip::Open(HINSTANCE instance, HWND owner) {
    impl_->Close(); impl_->instance = instance; impl_->owner = owner;
    if (!impl_->themeRead) impl_->ReadTheme();
    return impl_->Initialize() && impl_->Attach();
}
void TaskbarStrip::Close() { impl_->Close(); }
void TaskbarStrip::Configure(const Components& c, int zoom, TaskbarPosition position, int widgetsWidth) {
    if (impl_->components.taskbar != c.taskbar || impl_->zoom != zoom) impl_->layoutDirty = true;
    if (impl_->position != position) {
        impl_->autoSide = TaskbarPosition::Auto;
        impl_->dirty = true;
    }
    impl_->components = c; impl_->zoom = zoom;
    impl_->position = position; impl_->widgetsWidth = std::clamp(widgetsWidth, 0, 300);
}
void TaskbarStrip::ThemeChanged() { impl_->ReadTheme(); }
void TaskbarStrip::LanguageChanged() { impl_->layoutDirty = impl_->dirty = true; }
bool TaskbarStrip::Update(const Snapshot& snapshot) {
    if (!IsWindow(impl_->window) || !IsWindow(impl_->shell)) {
        // Missing Explorer is a real fallback; the owner retries on its existing
        // sampling timer or immediately on TaskbarCreated.
        impl_->Close();
        if (!impl_->Attach()) return false;
    }
    Components selection = impl_->components; selection.compact = selection.taskbar;
    if (impl_->model.Update(snapshot, true, selection, true)) impl_->dirty = true;
    if (!impl_->Track()) return false;
    if (IsWindowVisible(impl_->shell) && impl_->dirty && !impl_->Paint() && !impl_->Paint()) return false;
    impl_->loggedErrors.clear();
    return true;
}
bool TaskbarStrip::Visible() const {
    return impl_->shell && IsWindowVisible(impl_->shell);
}
RECT TaskbarStrip::ScreenRect() const {
    RECT r{};
    if (impl_->window) GetWindowRect(impl_->window, &r);
    return r;
}
} // namespace pulse::ui
