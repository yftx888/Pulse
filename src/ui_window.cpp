#include "ui_window.h"
#include "ui_renderer.h"
#include "ui_taskbar.h"
#include "metrics.h"
#include "netprobe.h"
#include <windowsx.h>
#include <shellapi.h>
#ifndef SEE_MASK_NOCLOSEPROCESS
#define SEE_MASK_NOCLOSEPROCESS 0x00000040
#endif
#ifndef SEE_MASK_NOASYNC
#define SEE_MASK_NOASYNC 0x00000100
#endif
#ifndef SEE_MASK_FLAG_NO_UI
#define SEE_MASK_FLAG_NO_UI 0x00000400
#endif
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <objbase.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cwchar>
#include <memory>
#include <utility>
#include <vector>

namespace pulse::ui {
namespace {
constexpr wchar_t MainClass[] = L"Pulse.Native.Main.v1";
constexpr wchar_t SingleInstanceMutex[] = L"Local\\Pulse.Native.SingleInstance.v1";
constexpr wchar_t RestartedArg[] = L"--restarted";
constexpr wchar_t PowerDiagnosticsArg[] = L"--power-diagnostics";
constexpr wchar_t ToastClass[] = L"Pulse.Native.Notification.v1";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kAppIconId = 1;
constexpr UINT SampleTimer = 1;
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT NetMessage = WM_APP + 2;
constexpr UINT ShowMessage = WM_APP + 3;
constexpr UINT ReapToastMessage = WM_APP + 4;
constexpr UINT ArrangeToastMessage = WM_APP + 5;
constexpr UINT DpiFitMessage = WM_APP + 6;
constexpr UINT CmdFull = 101, CmdCompact = 102, CmdPin = 103, CmdStartup = 104;
constexpr UINT CmdLog = 105, CmdExit = 106, CmdDefaults = 107, TrayId = 1;
constexpr UINT CmdNetworkNotify = 108, CmdNotifySound = 109, CmdRestartAdmin = 110;
constexpr UINT CmdShortcutDesktop = 111, CmdShortcutStartMenu = 112, CmdTaskbar = 113;
constexpr UINT CmdPowerDiagnostics = 114;
constexpr UINT CmdTaskbarComponent = 700;
constexpr UINT CmdTaskbarPosition = 750;
constexpr UINT CmdFullComponent = 200, CmdCompactComponent = 300;
constexpr UINT CmdRefresh = 400;
constexpr UINT CmdSlowThreshold = 450;
constexpr UINT CmdZoom = 500;
constexpr UINT CmdLanguage = 600;
constexpr std::array<i18n::Preference, 3> Languages{{
    i18n::Preference::Auto, i18n::Preference::Chinese, i18n::Preference::English}};
constexpr std::array<UINT, 3> RefreshIntervals{{500u, 1000u, 2000u}};
constexpr std::array<int, 7> SlowThresholds{{50, 100, 150, 200, 300, 500, 800}};
constexpr std::array<int, 6> ZoomLevels{{80, 100, 125, 150, 175, 200}};
constexpr DWORD MainStyle = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD ToastStyle = WS_POPUP | WS_CAPTION | WS_THICKFRAME;

void AllowShowFromLowerIntegrity(HWND window, UINT message = ShowMessage);

void AlertSound(bool play) {
    using PlayFn = BOOL(WINAPI*)(LPCWSTR, HMODULE, DWORD);
    static HMODULE module = nullptr;
    static PlayFn sound = nullptr;
    static bool tried = false;
    if (!play && !sound) return;
    if (play && !tried) {
        tried = true;
        module = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) sound = reinterpret_cast<PlayFn>(GetProcAddress(module, "PlaySoundW"));
    }
    if (!sound) return;
    if (!play) { sound(nullptr, nullptr, 0); return; }
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    const std::wstring file = std::wstring(directory) + L"\\Media\\Windows Notify System Generic.wav";
    sound(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

void SetSampleTimer(HWND window, bool compact, UINT fullInterval) {
    using CoalescableTimerFn = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC, ULONG);
    static const auto coalescable = reinterpret_cast<CoalescableTimerFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCoalescableTimer"));
    const UINT interval = compact ? 2000u : fullInterval;
    // Full mode prioritizes response time; keep compact mode's existing coalescing.
    const ULONG tolerance = compact ? 300u : 0xffffffffu; // TIMERV_NO_COALESCING
    if (!coalescable || !coalescable(window, SampleTimer, interval, nullptr, tolerance))
        SetTimer(window, SampleTimer, interval, nullptr);
}

int Pixels(float dip, UINT dpi, int zoom = 100) {
    return static_cast<int>(std::ceil(dip * dpi / 96.0f * zoom / 100.0f));
}
int ClampCoordinate(int value, int start, int end, int size) {
    return std::max(start, std::min(value, std::max(start, end - size)));
}
RECT WorkArea(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}
struct PaintSession {
    HWND window;
    PAINTSTRUCT state{};
    explicit PaintSession(HWND value) : window(value) { BeginPaint(window, &state); }
    ~PaintSession() { EndPaint(window, &state); }
};
std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min(size_t(32768), buffer.size() * 2));
    }
}
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_FALSE also increments COM's initialization count; failures do not.
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct ShellShortcut {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    ShellShortcut() = default;
    ShellShortcut(const ShellShortcut&) = delete;
    ShellShortcut& operator=(const ShellShortcut&) = delete;
    ~ShellShortcut() {
        if (file) file->Release();
        if (link) link->Release();
    }
    HRESULT Initialize() {
        HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(&link));
        if (SUCCEEDED(result))
            result = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
        return result;
    }
};
struct ShellPathDeleter {
    void operator()(wchar_t* path) const { CoTaskMemFree(path); }
};
HRESULT KnownFolderPath(REFKNOWNFOLDERID folder, DWORD flags, std::wstring& path) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(folder, flags, nullptr, &raw);
    const std::unique_ptr<wchar_t, ShellPathDeleter> memory(raw);
    if (FAILED(result)) return result;
    if (!raw || !*raw) return E_UNEXPECTED;
    path = raw;
    return S_OK;
}
std::wstring NormalizeShortcutPath(std::wstring path) {
    if (path.empty()) return path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (_wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0) path = L"\\\\" + path.substr(8);
    else if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    const DWORD fullSize = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (fullSize) {
        std::vector<wchar_t> full(fullSize);
        const DWORD length = GetFullPathNameW(path.c_str(), fullSize, full.data(), nullptr);
        if (length && length < fullSize) path.assign(full.data(), length);
    }
    // Compare short (8.3) and long spellings as the same executable when possible.
    const DWORD longSize = GetLongPathNameW(path.c_str(), nullptr, 0);
    if (longSize) {
        std::vector<wchar_t> expanded(longSize);
        const DWORD length = GetLongPathNameW(path.c_str(), expanded.data(), longSize);
        if (length && length < longSize) path.assign(expanded.data(), length);
    }
    return path;
}
HRESULT ShortcutLocationIsTemporary(const std::wstring& executable, bool& temporary) {
    const auto path = NormalizeShortcutPath(executable);
    auto lower = path;
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    temporary = lower.find(L"\\temp\\") != std::wstring::npos;
    if (temporary) return S_OK;
    std::wstring downloads;
    const HRESULT result = KnownFolderPath(FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, downloads);
    if (FAILED(result)) return result;
    downloads = NormalizeShortcutPath(std::move(downloads));
    if (downloads.back() != L'\\') downloads += L'\\';
    temporary = path.size() > downloads.size() &&
        CompareStringOrdinal(path.c_str(), static_cast<int>(downloads.size()),
            downloads.c_str(), static_cast<int>(downloads.size()), TRUE) == CSTR_EQUAL;
    return S_OK;
}
HRESULT ReadShortcutTarget(const std::wstring& path, std::wstring& target) {
    ShellShortcut shortcut;
    HRESULT result = shortcut.Initialize();
    if (SUCCEEDED(result)) result = shortcut.file->Load(path.c_str(), STGM_READ);
    if (FAILED(result)) return result;
    std::vector<wchar_t> buffer(32768, 0);
    // Do not Resolve: inspecting an old target must not launch a search or repair it.
    result = shortcut.link->GetPath(buffer.data(), static_cast<int>(buffer.size()), nullptr, 0);
    if (SUCCEEDED(result)) target = NormalizeShortcutPath(buffer.data());
    return result;
}
void CreateShortcut(HWND owner, REFKNOWNFOLDERID destination) {
    const auto fail = [owner](HRESULT result) {
        wchar_t code[16]{};
        std::swprintf(code, sizeof(code) / sizeof(code[0]), L"0x%08lX", static_cast<unsigned long>(result));
        const auto message = i18n::Format(Key::ShortcutFailed, {code});
        MessageBoxW(owner, message.c_str(), T(Key::AppName).c_str(), MB_OK | MB_ICONERROR);
    };
    ComApartment apartment;
    // A different apartment model means COM is already usable on this thread.
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) {
        fail(apartment.result);
        return;
    }
    const auto executable = ExecutablePath();
    if (executable.empty()) {
        const DWORD error = GetLastError();
        fail(HRESULT_FROM_WIN32(error ? error : ERROR_BAD_PATHNAME));
        return;
    }
    const auto slash = executable.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { fail(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME)); return; }
    std::wstring folder;
    HRESULT result = KnownFolderPath(destination, 0, folder);
    if (FAILED(result)) { fail(result); return; }
    if (folder.back() != L'\\') folder += L'\\';
    const auto shortcutPath = folder + L"Pulse.lnk";
    const DWORD attributes = GetFileAttributesW(shortcutPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) { fail(HRESULT_FROM_WIN32(ERROR_DIRECTORY)); return; }
        std::wstring target;
        result = ReadShortcutTarget(shortcutPath, target);
        if (FAILED(result)) { fail(result); return; }
        const auto current = NormalizeShortcutPath(executable);
        if (!target.empty() && CompareStringOrdinal(target.c_str(), -1, current.c_str(), -1, TRUE) == CSTR_EQUAL) {
            MessageBoxW(owner, T(Key::ShortcutExists).c_str(), T(Key::AppName).c_str(), MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (MessageBoxW(owner, T(Key::ShortcutReplace).c_str(), T(Key::AppName).c_str(),
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            fail(HRESULT_FROM_WIN32(error));
            return;
        }
    }
    bool temporary = false;
    result = ShortcutLocationIsTemporary(executable, temporary);
    if (FAILED(result)) { fail(result); return; }
    if (temporary && MessageBoxW(owner, T(Key::ShortcutLocationWarning).c_str(), T(Key::AppName).c_str(),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    // Use a fresh object so replacing a link cannot inherit old arguments or flags.
    ShellShortcut shortcut;
    result = shortcut.Initialize();
    if (SUCCEEDED(result)) result = shortcut.link->SetPath(executable.c_str());
    const auto directory = executable.substr(0, slash + 1); // Keep the backslash for a drive root.
    if (SUCCEEDED(result)) result = shortcut.link->SetWorkingDirectory(directory.c_str());
    if (SUCCEEDED(result)) result = shortcut.link->SetIconLocation(executable.c_str(), 0);
    if (SUCCEEDED(result)) result = shortcut.link->SetDescription(T(Key::AppName).c_str());
    if (SUCCEEDED(result)) result = shortcut.file->Save(shortcutPath.c_str(), TRUE);
    if (FAILED(result)) { fail(result); return; }
    MessageBoxW(owner, T(Key::ShortcutCreated).c_str(), T(Key::AppName).c_str(), MB_OK | MB_ICONINFORMATION);
}
HICON LoadPulseIcon(int xMetric, int yMetric, UINT dpi) {
    using MetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const auto metricsForDpi = reinterpret_cast<MetricsForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    const int cx = metricsForDpi ? metricsForDpi(xMetric, dpi) : GetSystemMetrics(xMetric);
    const int cy = metricsForDpi ? metricsForDpi(yMetric, dpi) : GetSystemMetrics(yMetric);
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId),
        IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}
HICON IconOrDefault(HICON icon) {
    // Keep the shared fallback out of the owned icon handles.
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

struct Config {
    int x = INT_MIN, y = INT_MIN;
    UINT refresh_ms = 1000;
    int slow_ms = 500;
    int zoom = 100;
    i18n::Preference language = i18n::Preference::Auto;
    DisplayMode mode = DisplayMode::Full;
    TaskbarPosition taskbarPosition = TaskbarPosition::Auto;
    int widgetsWidth = 120;
    bool pinned = true;
    bool notify = true, notify_sound = true;
    std::wstring file;
    Components components;
    bool ReadComponent(const wchar_t* section, const ComponentInfo& info) const {
        wchar_t value[16]{};
        GetPrivateProfileStringW(section, info.key, L"", value, 16, file.c_str());
        if (std::wcscmp(value, L"0") == 0) return false;
        if (std::wcscmp(value, L"1") == 0) return true;
        return info.enabled;
    }
    bool Load() {
        wchar_t directory[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, directory))) return false;
        std::wstring folder = std::wstring(directory) + L"\\Pulse";
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        file = folder + L"\\settings.ini";
        x = static_cast<int>(GetPrivateProfileIntW(L"Window", L"X", INT_MIN, file.c_str()));
        y = static_cast<int>(GetPrivateProfileIntW(L"Window", L"Y", INT_MIN, file.c_str()));
        wchar_t languageValue[32]{};
        GetPrivateProfileStringW(L"Window", L"language", L"auto", languageValue, 32, file.c_str());
        language = i18n::ParsePreference(languageValue);
        wchar_t modeValue[32]{};
        GetPrivateProfileStringW(L"Window", L"Mode", L"", modeValue, 32, file.c_str());
        if (std::wcscmp(modeValue, L"taskbar") == 0) mode = DisplayMode::Taskbar;
        else if (std::wcscmp(modeValue, L"compact") == 0) mode = DisplayMode::Compact;
        else if (std::wcscmp(modeValue, L"full") == 0) mode = DisplayMode::Full;
        else mode = GetPrivateProfileIntW(L"Window", L"Compact", 0, file.c_str()) ? DisplayMode::Compact : DisplayMode::Full;
        wchar_t positionValue[32]{};
        GetPrivateProfileStringW(L"Taskbar", L"position", L"auto", positionValue, 32, file.c_str());
        taskbarPosition = std::wcscmp(positionValue, L"right") == 0 ? TaskbarPosition::Right :
            std::wcscmp(positionValue, L"left") == 0 ? TaskbarPosition::Left : TaskbarPosition::Auto;
        wchar_t widgetsValue[32]{};
        GetPrivateProfileStringW(L"Taskbar", L"widgets_width", L"120", widgetsValue, 32, file.c_str());
        wchar_t* widgetsEnd = nullptr;
        const long widgetsDips = std::wcstol(widgetsValue, &widgetsEnd, 10);
        widgetsWidth = widgetsEnd != widgetsValue && *widgetsEnd == L'\0' ?
            static_cast<int>(std::clamp(widgetsDips, 0L, 300L)) : 120;
        pinned = GetPrivateProfileIntW(L"Window", L"OnTop", 1, file.c_str()) != 0;
        wchar_t zoomValue[32]{};
        GetPrivateProfileStringW(L"Window", L"zoom", L"100", zoomValue, 32, file.c_str());
        wchar_t* zoomEnd = nullptr;
        const long zoomPercent = std::wcstol(zoomValue, &zoomEnd, 10);
        zoom = zoomEnd != zoomValue && *zoomEnd == L'\0' && zoomPercent >= 80 && zoomPercent <= 200 ?
            static_cast<int>(zoomPercent) : 100;
        const UINT interval = GetPrivateProfileIntW(L"Window", L"refresh_ms", 1000, file.c_str());
        refresh_ms = std::find(RefreshIntervals.begin(), RefreshIntervals.end(), interval) != RefreshIntervals.end() ? interval : 1000u;
        wchar_t slow[32]{};
        GetPrivateProfileStringW(L"Network", L"slow_ms", L"500", slow, 32, file.c_str());
        slow_ms = 500;
        for (const int threshold : SlowThresholds)
            if (std::to_wstring(threshold) == slow) slow_ms = threshold;
        notify = GetPrivateProfileIntW(L"Network", L"notify", 1, file.c_str()) != 0;
        notify_sound = GetPrivateProfileIntW(L"Network", L"notify_sound", 1, file.c_str()) != 0;
        for (size_t i = 0; i < FullComponents.size(); ++i)
            components.full[i] = ReadComponent(L"Full", FullComponents[i]);
        for (size_t i = 0; i < CompactComponents.size(); ++i)
            components.compact[i] = ReadComponent(L"Compact", CompactComponents[i]);
        for (size_t i = 0; i < TaskbarComponents.size(); ++i)
            components.taskbar[i] = ReadComponent(L"Taskbar", TaskbarComponents[i]);
        return true;
    }
    void Save() const {
        if (file.empty()) return;
        const auto sx = std::to_wstring(x), sy = std::to_wstring(y);
        WritePrivateProfileStringW(L"Window", L"X", sx.c_str(), file.c_str());
        WritePrivateProfileStringW(L"Window", L"Y", sy.c_str(), file.c_str());
        WritePrivateProfileStringW(L"Window", L"Mode", mode == DisplayMode::Taskbar ? L"taskbar" :
            mode == DisplayMode::Compact ? L"compact" : L"full", file.c_str());
        WritePrivateProfileStringW(L"Window", L"Compact", mode != DisplayMode::Full ? L"1" : L"0", file.c_str());
        WritePrivateProfileStringW(L"Window", L"OnTop", pinned ? L"1" : L"0", file.c_str());
        WritePrivateProfileStringW(L"Window", L"language", i18n::PreferenceValue(language), file.c_str());
        WritePrivateProfileStringW(L"Taskbar", L"position", taskbarPosition == TaskbarPosition::Left ? L"left" :
            taskbarPosition == TaskbarPosition::Right ? L"right" : L"auto", file.c_str());
        const auto widgetsValue = std::to_wstring(widgetsWidth);
        WritePrivateProfileStringW(L"Taskbar", L"widgets_width", widgetsValue.c_str(), file.c_str());
        const auto zoomValue = std::to_wstring(zoom);
        WritePrivateProfileStringW(L"Window", L"zoom", zoomValue.c_str(), file.c_str());
        const auto interval = std::to_wstring(refresh_ms);
        WritePrivateProfileStringW(L"Window", L"refresh_ms", interval.c_str(), file.c_str());
        const auto slow = std::to_wstring(slow_ms);
        WritePrivateProfileStringW(L"Network", L"slow_ms", slow.c_str(), file.c_str());
        WritePrivateProfileStringW(L"Network", L"notify", notify ? L"1" : L"0", file.c_str());
        WritePrivateProfileStringW(L"Network", L"notify_sound", notify_sound ? L"1" : L"0", file.c_str());
        for (size_t i = 0; i < FullComponents.size(); ++i)
            WritePrivateProfileStringW(L"Full", FullComponents[i].key, components.full[i] ? L"1" : L"0", file.c_str());
        for (size_t i = 0; i < CompactComponents.size(); ++i)
            WritePrivateProfileStringW(L"Compact", CompactComponents[i].key, components.compact[i] ? L"1" : L"0", file.c_str());
        for (size_t i = 0; i < TaskbarComponents.size(); ++i)
            WritePrivateProfileStringW(L"Taskbar", TaskbarComponents[i].key, components.taskbar[i] ? L"1" : L"0", file.c_str());
    }
};

class Toast {
public:
    explicit Toast(NetEvent event, HWND notify) : event_(std::move(event)), message_(NetworkEventText(event_)), notify_(notify) {}
    ~Toast() { if (window_) DestroyWindow(window_); }
    bool Open(HINSTANCE instance, UINT dpi, int zoom) {
        renderer_.SetSoftware(true);
        if (FAILED(renderer_.Initialize(false))) return false;
        dpi_ = dpi;
        zoom_ = zoom;
        renderer_.SetZoom(zoom_);
        height_ = renderer_.ToastHeight(message_);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            ToastClass, T(Key::ToastCaption).c_str(), ToastStyle, 0, 0, Width(dpi_), Height(dpi_),
            nullptr, nullptr, instance, this);
        return window_ != nullptr;
    }
    void LanguageChanged() {
        renderer_.LanguageChanged();
        message_ = NetworkEventText(event_);
        height_ = renderer_.ToastHeight(message_, width_);
        if (window_) {
            SetWindowTextW(window_, T(Key::ToastCaption).c_str());
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    HWND Window() const { return window_; }
    int Width(UINT dpi) const { return Pixels(width_, dpi, zoom_); }
    int Height(UINT dpi) const { return Pixels(height_, dpi, zoom_); }
    void SetScale(UINT dpi, int zoom, int maxWidth) {
        const float scale = dpi / 96.0f * zoom / 100.0f;
        const float width = std::min(300.0f, std::max(1.0f, maxWidth / scale));
        if (dpi_ == dpi && zoom_ == zoom && width_ == width) return;
        dpi_ = dpi;
        zoom_ = zoom;
        renderer_.SetDpi(dpi_);
        renderer_.SetZoom(zoom_);
        width_ = width;
        height_ = renderer_.ToastHeight(message_, width_);
        if (window_) InvalidateRect(window_, nullptr, FALSE);
    }
    void Place(int x, int y, int height, UINT dpi) {
        dpi_ = dpi;
        renderer_.SetDpi(dpi);
        SetWindowPos(window_, HWND_TOPMOST, x, y, Width(dpi), height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!shown_) {
            shown_ = true;
            ArmExpiry(8000);
        }
        if (CursorInside()) PauseExpiry();
        else if (hovered_) {
            hovered_ = false;
            ArmExpiry(3000); // Arrangement/DPI changes may move the card away.
        }
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Toast*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Toast*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        try {
        switch (message) {
        case WM_CREATE:
            self->dpi_ = WindowDpi(window);
            self->renderer_.SetWindow(window, self->dpi_);
            StyleWindow(window);
            SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
            return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
        case WM_PAINT: {
            PaintSession paint(window);
            self->renderer_.PaintToast(self->event_, self->message_);
            return 0;
        }
        case WM_SIZE: self->renderer_.Resize(); return 0;
        case WM_DPICHANGED: {
            self->dpi_ = HIWORD(w);
            self->renderer_.SetDpi(self->dpi_);
            const auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, FALSE);
            PostMessageW(self->notify_, ArrangeToastMessage, 0, 0);
            return 0;
        }
        case WM_MOUSEMOVE:
            self->PauseExpiry();
            return 0;
        case WM_MOUSELEAVE:
            if (self->hovered_) {
                self->hovered_ = false;
                self->ArmExpiry(3000);
            }
            return 0;
        case WM_TIMER:
            self->OnTimer(w);
            return 0;
        case WM_DWMCOMPOSITIONCHANGED: StyleWindow(window); return 0;
        case WM_LBUTTONUP: case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_NCDESTROY:
            KillTimer(window, ExpireTimer);
            KillTimer(window, FadeTimer);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
            self->renderer_.DiscardTarget();
            PostMessageW(self->notify_, ReapToastMessage, 0, 0);
            break;
        }
        return DefWindowProcW(window, message, w, l);
        } catch (...) {
            if (message == WM_NCCREATE) return FALSE;
            if (message == WM_CREATE) return -1;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
private:
    static constexpr UINT_PTR ExpireTimer = 1, FadeTimer = 2;
    static constexpr ULONGLONG FadeDurationMs = 240;

    bool CursorInside() const {
        POINT cursor{};
        RECT rect{};
        return GetCursorPos(&cursor) && GetWindowRect(window_, &rect) &&
            PtInRect(&rect, cursor) && WindowFromPoint(cursor) == window_;
    }
    void ArmExpiry(UINT delay) {
        KillTimer(window_, ExpireTimer);
        KillTimer(window_, FadeTimer);
        fading_ = false;
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
        expires_at_ = GetTickCount64() + delay;
        if (!SetTimer(window_, ExpireTimer, delay, nullptr)) DestroyWindow(window_);
    }
    void PauseExpiry() {
        if (hovered_) return;
        hovered_ = true;
        KillTimer(window_, ExpireTimer);
        KillTimer(window_, FadeTimer);
        fading_ = false;
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
    }
    void OnTimer(UINT_PTR timer) {
        if (timer != ExpireTimer && timer != FadeTimer) return;
        // Check the actual cursor too: a card may appear under a stationary
        // pointer, or a queued WM_TIMER may arrive before WM_MOUSEMOVE.
        if (CursorInside()) { PauseExpiry(); return; }
        if (hovered_) return;
        const ULONGLONG now = GetTickCount64();
        if (timer == ExpireTimer) {
            KillTimer(window_, ExpireTimer); // Idle cards use one deadline, no polling.
            if (now < expires_at_) {
                SetTimer(window_, ExpireTimer, static_cast<UINT>(expires_at_ - now), nullptr);
                return;
            }
            fading_ = true;
            fade_started_ = now;
            if (!SetTimer(window_, FadeTimer, 30, nullptr)) DestroyWindow(window_);
            return;
        }
        if (!fading_) return; // Ignore a queued timer cancelled by a hover.
        const ULONGLONG elapsed = now - fade_started_;
        if (elapsed >= FadeDurationMs) {
            DestroyWindow(window_);
            return;
        }
        const BYTE alpha = static_cast<BYTE>(255 * (FadeDurationMs - elapsed) / FadeDurationMs);
        if (!SetLayeredWindowAttributes(window_, 0, alpha, LWA_ALPHA)) DestroyWindow(window_);
    }

    NetEvent event_;
    std::wstring message_; // Localized once on arrival or language change, never during paint.
    HWND window_ = nullptr, notify_ = nullptr;
    Renderer renderer_;
    UINT dpi_ = 96;
    int zoom_ = 100;
    float width_ = 300, height_ = 96;
    bool shown_ = false, hovered_ = false, fading_ = false;
    ULONGLONG expires_at_ = 0, fade_started_ = 0;
};

class Application {
public:
    explicit Application(HINSTANCE instance) : instance_(instance) {}
    ~Application() {
        callbackWindow_.store(nullptr, std::memory_order_release);
        metrics_.reset();
        taskbar_.Close();
        toasts_.clear();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        RemoveTray();
        if (registeredMain_) UnregisterClassW(MainClass, instance_);
        if (registeredToast_) UnregisterClassW(ToastClass, instance_);
        if (icon_) DestroyIcon(icon_);
        if (smallIcon_) DestroyIcon(smallIcon_);
        if (classIcon_) DestroyIcon(classIcon_);
        if (classSmallIcon_) DestroyIcon(classSmallIcon_);
    }
    int Execute(int showCommand, bool restarted) {
        config_.Load();
        taskbarFallback_ = config_.mode == DisplayMode::Taskbar;
        i18n::SetLanguage(config_.language);
        notificationState_.store(config_.notify ? 1u : 0u, std::memory_order_release);
        if (FAILED(renderer_.Initialize())) {
            MessageBoxW(nullptr, T(Key::GraphicsFailed).c_str(), L"Pulse", MB_OK | MB_ICONERROR);
            return 1;
        }
        POINT initial{config_.x == INT_MIN ? 0 : config_.x, config_.y == INT_MIN ? 0 : config_.y};
        const auto monitor = MonitorFromPoint(initial, config_.x == INT_MIN ? MONITOR_DEFAULTTOPRIMARY : MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        dpi_ = MonitorDpi(monitor);
        classIcon_ = LoadPulseIcon(SM_CXICON, SM_CYICON, dpi_);
        classSmallIcon_ = LoadPulseIcon(SM_CXSMICON, SM_CYSMICON, dpi_);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.style = CS_DBLCLKS;
        cls.lpfnWndProc = Procedure;
        cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = IconOrDefault(classIcon_);
        cls.hIconSm = IconOrDefault(classSmallIcon_);
        cls.lpszClassName = MainClass;
        if (!RegisterClassExW(&cls)) return 1;
        registeredMain_ = true;
        cls.lpfnWndProc = Toast::Procedure;
        cls.lpszClassName = ToastClass;
        if (!RegisterClassExW(&cls)) return 1;
        registeredToast_ = true;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        renderer_.SetSoftware(config_.mode != DisplayMode::Full);
        renderer_.SetZoom(config_.zoom);
        renderer_.SetComponents(config_.components, Compact());
        model_.Update(Snapshot{}, Compact(), config_.components);
        renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible);
        const auto size = FittedSize(work);
        const int width = static_cast<int>(size.cx), height = static_cast<int>(size.cy);
        if (config_.x == INT_MIN || config_.y == INT_MIN) {
            config_.x = work.right - width - Pixels(24, dpi_, config_.zoom);
            config_.y = work.top + Pixels(24, dpi_, config_.zoom);
        }
        config_.x = ClampCoordinate(config_.x, work.left, work.right, width);
        config_.y = ClampCoordinate(config_.y, work.top, work.bottom, height);
        window_ = CreateWindowExW(WS_EX_APPWINDOW, MainClass, T(Key::AppName).c_str(), MainStyle,
            config_.x, config_.y, width, height, nullptr, nullptr, instance_, this);
        if (!window_) return 1;
        AllowShowFromLowerIntegrity(window_);
        if (taskbarCreated_) AllowShowFromLowerIntegrity(window_, taskbarCreated_);
        callbackWindow_.store(window_, std::memory_order_release);
        Fit(false);
        SetWindowPos(window_, config_.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        AddTray();
        ConfigureTaskbar();
        if (config_.mode == DisplayMode::Taskbar) TrackTaskbar(true);
        metrics_ = std::make_unique<Metrics>();
        const Wants initialWants = CollectionWants(config_.components, CollectionMode(), details_, powerTrace_);
        metrics_->SetWants(initialWants); // Always before Start and the first Sample, including --restarted.
        if (initialWants.fps) {
            diagnostics::WriteLog(L"FPS", L"ui startup wants=1 compact=" +
                std::to_wstring(Compact() ? 1 : 0) + L" restarted=" + std::to_wstring(restarted ? 1 : 0) +
                L" full_fps=" + std::to_wstring(config_.components.Enabled(FullComponent::Fps) ? 1 : 0) +
                L" compact_fps=" + std::to_wstring(config_.components.Enabled(CompactComponent::Fps) ? 1 : 0));
        }
        Settings settings;
        settings.slow_ms = config_.slow_ms;
        metrics_->SetProbeEnabled(ProbeNeeded());
        metrics_->Start(settings, NetworkEvent, this);
        running_ = true;
        const bool startMinimized = showCommand == SW_SHOWMINIMIZED || showCommand == SW_MINIMIZE || showCommand == SW_SHOWMINNOACTIVE;
        if (!TaskbarMode())
            ShowWindow(window_, startMinimized ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
        Sampling(true);
        UpdateWindow(window_);
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return result < 0 ? 1 : static_cast<int>(message.wParam);
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        try { return self->Handle(message, w, l); }
        catch (...) {
            if (message == WM_NCCREATE) return FALSE;
            if (message == WM_CREATE) return -1;
            // Never unwind a C++ exception through a Win32 window procedure.
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
private:
    static void NetworkEvent(const NetEvent& event, void* user) noexcept {
        auto* app = static_cast<Application*>(user);
        const UINT_PTR notificationState = app->notificationState_.load(std::memory_order_acquire);
        if (!(notificationState & 1u)) return;
        const HWND window = app->callbackWindow_.load(std::memory_order_acquire);
        if (!window) return;
        try {
            auto copy = std::make_unique<NetEvent>(event);
            if (PostMessageW(window, NetMessage, notificationState, reinterpret_cast<LPARAM>(copy.get()))) copy.release();
        } catch (...) {}
    }
    bool TaskbarMode() const { return config_.mode == DisplayMode::Taskbar && !taskbarFallback_; }
    bool Compact() const { return config_.mode == DisplayMode::Compact || taskbarFallback_; }
    DisplayMode CollectionMode() const { return taskbarFallback_ ? DisplayMode::Compact : config_.mode; }
    bool Cheap() const { return config_.mode != DisplayMode::Full && !details_; }
    void ConfigureTaskbar() {
        taskbar_.Configure(config_.components, config_.zoom, config_.taskbarPosition, config_.widgetsWidth);
    }
    bool ProbeNeeded() const {
        if (config_.notify) return true;
        if (TaskbarMode()) {
            return (taskbar_.Visible() && config_.components.taskbar[static_cast<size_t>(CompactComponent::Latency)]) ||
                (details_ && IsWindowVisible(window_) && !IsIconic(window_) && config_.components.Enabled(FullComponent::Network));
        }
        if (!window_ || !IsWindowVisible(window_) || IsIconic(window_)) return false;
        return Compact() ? config_.components.Enabled(CompactComponent::Latency) :
                           config_.components.Enabled(FullComponent::Network);
    }
    void UpdateCollection() {
        if (!metrics_) return;
        metrics_->SetWants(CollectionWants(config_.components, CollectionMode(), details_, powerTrace_));
        metrics_->SetProbeEnabled(ProbeNeeded());
    }
    void PositionDetails() {
        if (!TaskbarMode() || !details_ || fitting_) return;
        const RECT strip = taskbar_.ScreenRect();
        const RECT work = WorkArea(MonitorFromRect(&strip, MONITOR_DEFAULTTOPRIMARY));
        const UINT dpi = MonitorDpi(MonitorFromRect(&strip, MONITOR_DEFAULTTOPRIMARY));
        dpi_ = dpi;
        renderer_.SetDpi(dpi_);
        const auto size = FittedSize(work);
        const int x = ClampCoordinate(strip.right - size.cx, work.left, work.right, size.cx);
        const int y = ClampCoordinate(std::min(strip.top, work.bottom) - size.cy - 2, work.top, work.bottom, size.cy);
        RECT current{};
        GetWindowRect(window_, &current);
        if (current.left == x && current.top == y && current.right - current.left == size.cx &&
            current.bottom - current.top == size.cy) return;
        fitting_ = true;
        SetWindowPos(window_, config_.pinned ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
        fitting_ = false;
    }
    void ToggleDetails() {
        if (!TaskbarMode()) return;
        details_ = !details_;
        renderer_.SetTaskbarDetails(details_);
        if (!details_) {
            ShowWindow(window_, SW_HIDE);
            renderer_.SetSoftware(true);
        } else {
            renderer_.SetSoftware(false);
            renderer_.SetComponents(config_.components, false);
            PositionDetails();
            ShowWindow(window_, SW_SHOWNORMAL);
            SetForegroundWindow(window_);
        }
        Reconfigure(true);
        PositionDetails();
    }
    void SetTaskbarFallback(bool fallback) {
        if (taskbarFallback_ == fallback) return;
        CapturePosition();
        taskbarFallback_ = fallback;
        details_ = false;
        renderer_.SetTaskbarDetails(false);
        renderer_.SetSoftware(true);
        renderer_.SetComponents(config_.components, Compact());
        hover_ = pressed_ = Button::None;
        dragging_ = dragCandidate_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        scroll_ = 0;
        renderer_.SetScroll(0);
        model_.Update(latest_, Compact(), config_.components);
        renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible);
        repaintPending_ = true;
        if (fallback) {
            SetWindowPos(window_, nullptr, config_.x, config_.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Fit(false);
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            InvalidateRect(window_, nullptr, FALSE);
        } else ShowWindow(window_, SW_HIDE);
        Sampling(false, true);
    }
    void TrackTaskbar(bool retryNow = false) {
        if (config_.mode != DisplayMode::Taskbar) return;
        const ULONGLONG now = GetTickCount64();
        if (taskbarFallback_ && !retryNow && now < taskbarRetryAt_) return;
        bool ready = true;
        if (taskbarFallback_ || retryNow) {
            ConfigureTaskbar();
            ready = taskbar_.Open(instance_, window_);
        }
        if (ready) ready = taskbar_.Update(latest_);
        if (!ready) {
            taskbar_.Close();
            taskbarRetryAt_ = GetTickCount64() + 30000;
            SetTaskbarFallback(true);
            return;
        }
        const bool recovered = taskbarRetryAt_ != 0;
        taskbarRetryAt_ = 0;
        SetTaskbarFallback(false);
        if (recovered) diagnostics::WriteLog(L"UI", L"Taskbar attach retry succeeded; returning to taskbar mode");
        if (details_) PositionDetails();
    }
    float ContentWidth() const { return Compact() ? renderer_.CompactWidth() : renderer_.FullWidth(); }
    float ContentHeight() const { return Compact() ? 40 : FullLayout(config_.components, renderer_.FullWidth()).height; }
    float Scale() const { return dpi_ / 96.0f * config_.zoom / 100.0f; }
    SIZE FittedSize(const RECT& work) const {
        return {std::min(Pixels(ContentWidth(), dpi_, config_.zoom), std::max(1, static_cast<int>(work.right - work.left))),
                std::min(Pixels(ContentHeight(), dpi_, config_.zoom), std::max(1, static_cast<int>(work.bottom - work.top)))};
    }
    float CurrentWidth() const {
        RECT r{};
        GetClientRect(window_, &r);
        return r.right / Scale();
    }
    Button Hit(LPARAM l) const {
        return renderer_.HitTest(GET_X_LPARAM(l) / Scale(), GET_Y_LPARAM(l) / Scale(), Compact(), CurrentWidth());
    }
    void CapturePosition() {
        if (!window_ || TaskbarMode() || IsIconic(window_)) return;
        RECT r{};
        if (GetWindowRect(window_, &r)) { config_.x = r.left; config_.y = r.top; }
    }
    void Save() { CapturePosition(); config_.Save(); }
    void Fit(bool anchor, const RECT* suggestion = nullptr, bool topRight = false) {
        if (!window_ || fitting_) return;
        if (TaskbarMode()) { PositionDetails(); return; }
        if (IsIconic(window_)) {
            if (topRight && !zoomFitPending_) {
                WINDOWPLACEMENT placement{};
                placement.length = sizeof(placement);
                if (GetWindowPlacement(window_, &placement)) {
                    zoomAnchor_ = placement.rcNormalPosition;
                    // Main windows use workspace coordinates in WINDOWPLACEMENT.
                    const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO info{};
                    info.cbSize = sizeof(info);
                    if (GetMonitorInfoW(monitor, &info))
                        OffsetRect(&zoomAnchor_, info.rcWork.left - info.rcMonitor.left,
                                   info.rcWork.top - info.rcMonitor.top);
                    zoomFitPending_ = true;
                }
            }
            return;
        }
        topRight = topRight || zoomFitPending_;
        RECT old{};
        GetWindowRect(window_, &old);
        RECT base = zoomFitPending_ ? zoomAnchor_ : suggestion ? *suggestion : old;
        zoomFitPending_ = false;
        const auto monitor = MonitorFromRect(&base, MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        const auto size = FittedSize(work);
        const int width = static_cast<int>(size.cx), height = static_cast<int>(size.cy);
        int x = base.left, y = base.top;
        if (topRight) x = base.right - width;
        else if (anchor && !suggestion) {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            const RECT screen = GetMonitorInfoW(monitor, &info) ? info.rcMonitor : work;
            if (static_cast<long long>(old.left) + old.right > static_cast<long long>(screen.left) + screen.right) x = old.right - width;
            if (static_cast<long long>(old.top) + old.bottom > static_cast<long long>(screen.top) + screen.bottom) y = old.bottom - height;
        }
        x = ClampCoordinate(x, work.left, work.right, width);
        y = ClampCoordinate(y, work.top, work.bottom, height);
        ClampScroll((Compact() ? width : height) / Scale());
        if (old.left == x && old.top == y && old.right - old.left == width && old.bottom - old.top == height) return;
        fitting_ = true;
        SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        fitting_ = false;
        CapturePosition();
    }
    void ClampScroll(float viewport = 0) {
        if (viewport <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            viewport = (Compact() ? client.right : client.bottom) / Scale();
        }
        const float extent = Compact() ? ContentWidth() : ContentHeight();
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, extent - viewport));
        renderer_.SetScroll(scroll_);
    }
    Snapshot ReadMetrics() {
        auto sample = metrics_->Sample(!Cheap());
        fpsStatus_ = sample.fps_status;
        // sample.power_trace stays the observed value, including Unknown.
        const PowerTrace next = NextCollectionTrace(powerTrace_, sample.power_trace);
        if (powerTrace_ != next) {
            powerTrace_ = next;
            metrics_->SetWants(CollectionWants(config_.components, CollectionMode(), details_, powerTrace_));
        }
        return sample;
    }
    void Refresh() {
        if (!running_ || !active_ || !metrics_) return;
        if (config_.mode == DisplayMode::Taskbar) UpdateCollection();
        const auto sample = ReadMetrics();
        latest_ = sample;
        TrackTaskbar();
        if (TaskbarMode() && !details_) return;
        if (model_.Update(sample, Compact(), config_.components) || renderLost_) repaintPending_ = true;
        if (renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible)) Fit(true);
        // Keep sampling while covered, then draw the newest view when exposed.
        // Checking once per sample also recovers if no exposure WM_PAINT arrives.
        if (repaintPending_ && renderer_.CanRender()) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    void Sampling(bool sampleNow, bool resetTimer = false) {
        if (!running_ || !metrics_) return;
        const bool active = !systemSuspended_ && (config_.mode == DisplayMode::Taskbar ||
            (IsWindowVisible(window_) && !IsIconic(window_)));
        const bool changed = active_ != active;
        if (changed || resetTimer || !active) KillTimer(window_, SampleTimer);
        UpdateCollection();
        metrics_->SetPaused(!active);
        active_ = active;
        if (!active) {
            // Pausing stops the timer, so Take would otherwise never get a chance
            // to release an active FPS session until the window was shown again.
            if (changed) metrics_->Sample(!Cheap());
            return;
        }
        if (changed) model_.BreakHistory();
        if (changed || resetTimer) SetSampleTimer(window_, Cheap(), config_.refresh_ms);
        if (sampleNow || changed) Refresh();
    }
    void Reconfigure(bool resetHistory, bool visibleChanged = true) {
        UpdateCollection();
        ConfigureTaskbar();
        if (!visibleChanged) { Save(); return; }
        hover_ = pressed_ = Button::None;
        scroll_ = 0;
        renderer_.SetScroll(0);
        renderer_.SetComponents(config_.components, Compact());
        if (resetHistory) model_.BreakHistory();
        const Snapshot sample = metrics_ ? ReadMetrics() : Snapshot{};
        latest_ = sample;
        TrackTaskbar();
        model_.Update(sample, Compact(), config_.components);
        renderer_.SetCompactEtaVisible(model_.Data().compactEtaVisible);
        repaintPending_ = true;
        Fit(true);
        Sampling(false, true);
        Save();
        if (active_) InvalidateRect(window_, nullptr, FALSE);
    }
    void SetLanguage(i18n::Preference preference) {
        const bool preferenceChanged = config_.language != preference;
        config_.language = preference;
        if (!i18n::SetLanguage(preference)) {
            if (preferenceChanged) Save();
            return;
        }
        renderer_.LanguageChanged();
        taskbar_.LanguageChanged();
        Reconfigure(false); // Rebuild view, fixed widths, HWND size and text layouts once.
        for (auto& toast : toasts_) toast->LanguageChanged();
        ArrangeToasts();
        if (tray_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = TrayId;
            data.uFlags = NIF_TIP | NIF_SHOWTIP;
            std::swprintf(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), L"%ls", T(Key::TrayTip).c_str());
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
    }
    void SetMode(DisplayMode mode) {
        if (config_.mode == mode) {
            if (mode == DisplayMode::Taskbar && taskbarFallback_) TrackTaskbar(true);
            return;
        }
        CapturePosition();
        const bool fromTaskbar = config_.mode == DisplayMode::Taskbar;
        const POINT floating{config_.x, config_.y};
        details_ = false;
        renderer_.SetTaskbarDetails(false);
        ShowWindow(window_, SW_HIDE);
        taskbar_.Close();
        taskbarFallback_ = false;
        taskbarRetryAt_ = 0;
        config_.mode = mode;
        if (mode == DisplayMode::Taskbar) {
            taskbarFallback_ = true;
            ConfigureTaskbar();
            TrackTaskbar(true);
        }
        renderer_.SetSoftware(config_.mode != DisplayMode::Full);
        Reconfigure(true);
        if (!TaskbarMode()) {
            if (fromTaskbar) SetWindowPos(window_, nullptr, floating.x, floating.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Show();
            Save();
        }
        Sampling(true, true);
    }
    void SetCompact(bool compact) { SetMode(compact ? DisplayMode::Compact : DisplayMode::Full); }
    void SetZoom(int percent) {
        percent = std::clamp(percent, 80, 200);
        if (config_.zoom == percent) return;
        config_.zoom = percent;
        renderer_.SetZoom(percent);
        ConfigureTaskbar();
        TrackTaskbar();
        PositionDetails();
        hover_ = pressed_ = Button::None;
        dragging_ = dragCandidate_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        repaintPending_ = true;
        Fit(false, nullptr, true); // Preserve the right/top edges, then clamp to this monitor.
        ArrangeToasts();
        Save();
        if (!IsIconic(window_)) InvalidateRect(window_, nullptr, FALSE);
    }
    void TogglePin() {
        config_.pinned = !config_.pinned;
        SetWindowPos(window_, config_.pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Save();
        if (active_) InvalidateRect(window_, nullptr, FALSE);
    }
    void Show() {
        if (TaskbarMode()) { if (!details_) ToggleDetails(); return; }
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        Fit(false);
        SetForegroundWindow(window_);
        Sampling(true);
    }
    void ToggleVisibility() {
        if (TaskbarMode()) { ToggleDetails(); return; }
        if (IsWindowVisible(window_) && !IsIconic(window_)) {
            Save();
            ShowWindow(window_, SW_HIDE);
            Sampling(false);
        } else Show();
    }
    void UpdateIcons() {
        const HICON oldIcon = std::exchange(icon_, LoadPulseIcon(SM_CXICON, SM_CYICON, dpi_));
        const HICON oldSmallIcon = std::exchange(smallIcon_, LoadPulseIcon(SM_CXSMICON, SM_CYSMICON, dpi_));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(IconOrDefault(icon_)));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(IconOrDefault(smallIcon_)));
        if (tray_) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = TrayId;
            data.uFlags = NIF_ICON;
            data.hIcon = IconOrDefault(smallIcon_);
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        if (oldIcon) DestroyIcon(oldIcon);
        if (oldSmallIcon) DestroyIcon(oldSmallIcon);
    }
    void AddTray() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = TrayId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = TrayMessage;
        data.hIcon = IconOrDefault(smallIcon_);
        std::swprintf(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), L"%ls", T(Key::TrayTip).c_str());
        tray_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (tray_) {
            data.uVersion = NOTIFYICON_VERSION_4;
            trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        }
    }
    void RemoveTray() {
        if (!tray_) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = TrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_ = false;
    }
    bool StartupEnabled() const {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
        DWORD type = 0, bytes = 0;
        const LSTATUS status = RegQueryValueExW(key, L"Pulse", nullptr, &type, nullptr, &bytes);
        bool enabled = false;
        if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
            std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, 0);
            if (RegQueryValueExW(key, L"Pulse", nullptr, nullptr, reinterpret_cast<BYTE*>(value.data()), &bytes) == ERROR_SUCCESS) {
                const auto expected = L"\"" + ExecutablePath() + L"\"";
                enabled = _wcsicmp(value.data(), expected.c_str()) == 0;
            }
        }
        RegCloseKey(key);
        return enabled;
    }
    void ToggleStartup() {
        const bool disable = StartupEnabled();
        HKEY key = nullptr;
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0,
                                         KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status == ERROR_SUCCESS) {
            if (disable) {
                status = RegDeleteValueW(key, L"Pulse");
                if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
            } else {
                const auto executable = ExecutablePath();
                const auto command = L"\"" + executable + L"\"";
                status = executable.empty() ? ERROR_FILE_NOT_FOUND : RegSetValueExW(key, L"Pulse", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            }
            RegCloseKey(key);
        }
        if (status != ERROR_SUCCESS) MessageBoxW(window_, T(Key::StartupSettingFailed).c_str(), L"Pulse", MB_OK | MB_ICONWARNING);
    }
    void OpenLog() {
        if (!metrics_) return;
        const auto log = metrics_->LogPath();
        if (log.empty()) {
            MessageBoxW(window_, T(Key::LogNotReady).c_str(), L"Pulse", MB_OK | MB_ICONINFORMATION);
            return;
        }
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            const auto parameter = L"\"" + log + L"\"";
            result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", L"notepad.exe", parameter.c_str(), nullptr, SW_SHOWNORMAL));
        }
        if (result <= 32) MessageBoxW(window_, T(Key::LogOpenFailed).c_str(), L"Pulse", MB_OK | MB_ICONINFORMATION);
    }
    void StartPowerDiagnostics() {
        const auto path = ExecutablePath();
        if (path.empty()) {
            MessageBoxW(window_, i18n::Format(Key::PowerDiagnosticsLaunchFailed, {L"0"}).c_str(),
                T(Key::AppName).c_str(), MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring command = L"\"" + path + L"\" ";
        command += PowerDiagnosticsArg;
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &startup, &process)) {
            MessageBoxW(window_, i18n::Format(Key::PowerDiagnosticsLaunchFailed,
                {std::to_wstring(GetLastError())}).c_str(), T(Key::AppName).c_str(), MB_OK | MB_ICONWARNING);
            return;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    bool FpsNeedsAdmin() const {
        if (fpsStatus_ != 2) return false;
        if (TaskbarMode() && config_.components.taskbar[static_cast<size_t>(CompactComponent::Fps)]) return true;
        return Compact() ? config_.components.Enabled(CompactComponent::Fps)
                               : config_.components.Enabled(FullComponent::Fps);
    }
    void RestartElevated() {
        const auto path = ExecutablePath();
        if (path.empty() || !window_) return;
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        // NOASYNC returns after the new process is created, not after it exits.
        // Do not wait on hProcess: the new instance waits for this mutex.
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.hwnd = window_;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = RestartedArg;
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) return;
        if (info.hProcess) CloseHandle(info.hProcess);
        SendMessageW(window_, WM_CLOSE, 0, 0);
    }
    void Menu(POINT point) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | (config_.mode == DisplayMode::Full ? MF_CHECKED : 0), CmdFull, T(Key::FullMode).c_str());
        AppendMenuW(menu, MF_STRING | (Compact() ? MF_CHECKED : 0), CmdCompact, T(Key::MiniMode).c_str());
        AppendMenuW(menu, MF_STRING | (config_.mode == DisplayMode::Taskbar ? MF_CHECKED : 0), CmdTaskbar, T(Key::TaskbarMode).c_str());
        const UINT selectedMode = config_.mode == DisplayMode::Taskbar ? CmdTaskbar : Compact() ? CmdCompact : CmdFull;
        for (UINT id : {CmdFull, CmdCompact, CmdTaskbar}) {
            MENUITEMINFOW info{}; info.cbSize = sizeof(info); info.fMask = MIIM_FTYPE | MIIM_STATE;
            info.fType = MFT_STRING | MFT_RADIOCHECK; info.fState = id == selectedMode ? MFS_CHECKED : MFS_UNCHECKED;
            SetMenuItemInfoW(menu, id, FALSE, &info);
        }
        HMENU position = CreatePopupMenu();
        if (position) {
            const Key labels[] = {Key::TaskbarPositionAuto, Key::TaskbarPositionRight, Key::TaskbarPositionLeft};
            for (UINT i = 0; i < 3; ++i)
                AppendMenuW(position, MF_STRING, CmdTaskbarPosition + i, T(labels[i]).c_str());
            CheckMenuRadioItem(position, CmdTaskbarPosition, CmdTaskbarPosition + 2,
                CmdTaskbarPosition + static_cast<UINT>(config_.taskbarPosition), MF_BYCOMMAND);
            if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(position), T(Key::TaskbarPositionMenu).c_str()))
                DestroyMenu(position);
        }
        HMENU language = CreatePopupMenu();
        if (language) {
            const Key labels[] = {Key::LanguageAuto, Key::LanguageZh, Key::LanguageEn};
            UINT selected = CmdLanguage;
            for (size_t i = 0; i < Languages.size(); ++i) {
                const UINT id = CmdLanguage + static_cast<UINT>(i);
                AppendMenuW(language, MF_STRING, id, T(labels[i]).c_str());
                if (config_.language == Languages[i]) selected = id;
            }
            CheckMenuRadioItem(language, CmdLanguage, CmdLanguage + 2, selected, MF_BYCOMMAND);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language), T(Key::LanguageMenu).c_str());
        }
        HMENU zoom = CreatePopupMenu();
        if (zoom) {
            for (size_t i = 0; i < ZoomLevels.size(); ++i) {
                auto label = std::to_wstring(ZoomLevels[i]) + L"%";
                if (ZoomLevels[i] == 100) label += T(Key::DefaultSuffix);
                AppendMenuW(zoom, MF_STRING, CmdZoom + static_cast<UINT>(i), label.c_str());
            }
            const auto selected = std::find(ZoomLevels.begin(), ZoomLevels.end(), config_.zoom);
            if (selected != ZoomLevels.end())
                CheckMenuRadioItem(zoom, CmdZoom, CmdZoom + static_cast<UINT>(ZoomLevels.size()) - 1,
                                   CmdZoom + static_cast<UINT>(selected - ZoomLevels.begin()), MF_BYCOMMAND);
            const auto label = selected == ZoomLevels.end() ?
                i18n::Format(Key::CustomSize, {std::to_wstring(config_.zoom)}) : T(Key::Size);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoom), label.c_str());
        }
        HMENU refresh = CreatePopupMenu();
        if (refresh) {
            const wchar_t* labels[] = {T(Key::RefreshHalf).c_str(), T(Key::RefreshOne).c_str(), T(Key::RefreshTwo).c_str()};
            for (size_t i = 0; i < RefreshIntervals.size(); ++i)
                AppendMenuW(refresh, MF_STRING | (config_.refresh_ms == RefreshIntervals[i] ? MF_CHECKED : 0),
                            CmdRefresh + static_cast<UINT>(i), labels[i]);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(refresh), T(Key::RefreshInterval).c_str());
        }
        HMENU slow = CreatePopupMenu();
        if (slow) {
            UINT selected = CmdSlowThreshold + 5;
            for (size_t i = 0; i < SlowThresholds.size(); ++i) {
                const UINT id = CmdSlowThreshold + static_cast<UINT>(i);
                auto label = std::to_wstring(SlowThresholds[i]) + T(Key::MsSuffix);
                if (SlowThresholds[i] == 500) label += T(Key::DefaultSuffix);
                AppendMenuW(slow, MF_STRING, id, label.c_str());
                if (config_.slow_ms == SlowThresholds[i]) selected = id;
            }
            CheckMenuRadioItem(slow, CmdSlowThreshold,
                              CmdSlowThreshold + static_cast<UINT>(SlowThresholds.size()) - 1,
                              selected, MF_BYCOMMAND);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(slow), T(Key::SlowThreshold).c_str());
        }
        AppendMenuW(menu, MF_STRING | (config_.notify ? MF_CHECKED : 0), CmdNetworkNotify, T(Key::DisconnectAlerts).c_str());
        AppendMenuW(menu, MF_STRING | (config_.notify_sound ? MF_CHECKED : 0) |
                    (config_.notify ? 0 : MF_GRAYED), CmdNotifySound, T(Key::AlertSound).c_str());
        HMENU contents = CreatePopupMenu();
        HMENU full = CreatePopupMenu();
        HMENU compact = CreatePopupMenu();
        HMENU taskbar = CreatePopupMenu();
        if (contents && full && compact && taskbar) {
            for (size_t i = 0; i < FullComponents.size(); ++i)
                AppendMenuW(full, MF_STRING | (config_.components.full[i] ? MF_CHECKED : 0),
                            CmdFullComponent + static_cast<UINT>(i), T(FullComponents[i].label).c_str());
            for (size_t i = 0; i < CompactComponents.size(); ++i)
                AppendMenuW(compact, MF_STRING | (config_.components.compact[i] ? MF_CHECKED : 0),
                            CmdCompactComponent + static_cast<UINT>(i), T(CompactComponents[i].label).c_str());
            for (size_t i = 0; i < TaskbarComponents.size(); ++i)
                AppendMenuW(taskbar, MF_STRING | (config_.components.taskbar[i] ? MF_CHECKED : 0),
                    CmdTaskbarComponent + static_cast<UINT>(i), T(TaskbarComponents[i].label).c_str());
            AppendMenuW(contents, MF_POPUP, reinterpret_cast<UINT_PTR>(taskbar), T(Key::TaskbarMode).c_str());
            AppendMenuW(contents, MF_POPUP, reinterpret_cast<UINT_PTR>(full), T(Key::FullMode).c_str());
            AppendMenuW(contents, MF_POPUP, reinterpret_cast<UINT_PTR>(compact), T(Key::MiniMode).c_str());
            AppendMenuW(contents, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(contents, MF_STRING, CmdDefaults, T(Key::ResetDefaults).c_str());
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(contents), T(Key::ShowItems).c_str());
        } else {
            if (contents) DestroyMenu(contents);
            if (full) DestroyMenu(full);
            if (compact) DestroyMenu(compact);
            if (taskbar) DestroyMenu(taskbar);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (config_.pinned ? MF_CHECKED : 0), CmdPin, T(Key::AlwaysOnTop).c_str());
        AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), CmdStartup, T(Key::StartWithWindows).c_str());
        HMENU shortcuts = CreatePopupMenu();
        if (shortcuts) {
            AppendMenuW(shortcuts, MF_STRING, CmdShortcutDesktop, T(Key::ShortcutDesktop).c_str());
            AppendMenuW(shortcuts, MF_STRING, CmdShortcutStartMenu, T(Key::ShortcutStartMenu).c_str());
            if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(shortcuts), T(Key::CreateShortcut).c_str()))
                DestroyMenu(shortcuts);
        }
        AppendMenuW(menu, MF_STRING, CmdPowerDiagnostics, T(Key::PowerDiagnostics).c_str());
        AppendMenuW(menu, MF_STRING, CmdLog, T(Key::OpenLog).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (FpsNeedsAdmin())
            AppendMenuW(menu, MF_STRING, CmdRestartAdmin, T(Key::RestartAdmin).c_str());
        AppendMenuW(menu, MF_STRING, CmdExit, T(Key::Exit).c_str());
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                              point.x, point.y, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command) Command(command);
    }
    void Command(UINT command) {
        if (command >= CmdTaskbarPosition && command < CmdTaskbarPosition + 3) {
            config_.taskbarPosition = static_cast<TaskbarPosition>(command - CmdTaskbarPosition);
            ConfigureTaskbar();
            TrackTaskbar();
            Save();
            return;
        }
        if (command >= CmdLanguage && command < CmdLanguage + static_cast<UINT>(Languages.size())) {
            SetLanguage(Languages[command - CmdLanguage]);
            return;
        }
        if (command >= CmdZoom && command < CmdZoom + static_cast<UINT>(ZoomLevels.size())) {
            zoomWheelDelta_ = 0;
            SetZoom(ZoomLevels[command - CmdZoom]);
            return;
        }
        if (command >= CmdSlowThreshold && command < CmdSlowThreshold + static_cast<UINT>(SlowThresholds.size())) {
            config_.slow_ms = SlowThresholds[command - CmdSlowThreshold];
            if (metrics_) metrics_->SetSlowThreshold(config_.slow_ms);
            Refresh(); // Repaint the reclassified result immediately; no new HTTP probe.
            Save();
            return;
        }
        if (command >= CmdRefresh && command < CmdRefresh + static_cast<UINT>(RefreshIntervals.size())) {
            config_.refresh_ms = RefreshIntervals[command - CmdRefresh];
            if (!Cheap()) Sampling(false, true);
            Save();
            return;
        }
        if (command >= CmdFullComponent && command < CmdFullComponent + static_cast<UINT>(FullComponents.size())) {
            const size_t i = command - CmdFullComponent;
            config_.components.full[i] = !config_.components.full[i];
            Reconfigure(false, !Compact() && (!TaskbarMode() || details_));
            return;
        }
        if (command >= CmdCompactComponent && command < CmdCompactComponent + static_cast<UINT>(CompactComponents.size())) {
            const size_t i = command - CmdCompactComponent;
            config_.components.compact[i] = !config_.components.compact[i];
            Reconfigure(false, Compact());
            return;
        }
        if (command >= CmdTaskbarComponent && command < CmdTaskbarComponent + static_cast<UINT>(TaskbarComponents.size())) {
            const size_t i = command - CmdTaskbarComponent;
            config_.components.taskbar[i] = !config_.components.taskbar[i];
            Reconfigure(false, config_.mode == DisplayMode::Taskbar);
            return;
        }
        switch (command) {
        case CmdTaskbar: SetMode(DisplayMode::Taskbar); break;
        case CmdNetworkNotify:
            config_.notify = !config_.notify;
            notificationState_.fetch_add(1u, std::memory_order_acq_rel);
            ClearNetworkNotifications(); // Dismiss cards and discard queued events on either toggle.
            if (!config_.notify) AlertSound(false);
            UpdateCollection();
            Save();
            break;
        case CmdNotifySound:
            if (config_.notify) {
                config_.notify_sound = !config_.notify_sound;
                if (!config_.notify_sound) AlertSound(false);
                Save();
            }
            break;
        case CmdDefaults: config_.components.Reset(); Reconfigure(true); break;
        case CmdFull: SetCompact(false); break;
        case CmdCompact: SetCompact(true); break;
        case CmdPin: TogglePin(); break;
        case CmdStartup: ToggleStartup(); break;
        case CmdShortcutDesktop: CreateShortcut(window_, FOLDERID_Desktop); break;
        case CmdShortcutStartMenu: CreateShortcut(window_, FOLDERID_Programs); break;
        case CmdLog: OpenLog(); break;
        case CmdPowerDiagnostics: StartPowerDiagnostics(); break;
        case CmdRestartAdmin: RestartElevated(); break;
        case CmdExit: SendMessageW(window_, WM_CLOSE, 0, 0); break;
        }
    }
    void ActivateButton(Button button) {
        switch (button) {
        case Button::Mode: if (TaskbarMode()) ToggleDetails(); else SetCompact(!Compact()); break;
        case Button::Pin: TogglePin(); break;
        case Button::Minimize: if (TaskbarMode()) ToggleDetails(); else { ShowWindow(window_, SW_MINIMIZE); Sampling(false); } break;
        case Button::Close: SendMessageW(window_, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }
    void ArrangeToasts() {
        const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        const UINT dpi = MonitorDpi(monitor);
        const int margin = Pixels(16, dpi, config_.zoom), gap = Pixels(10, dpi, config_.zoom);
        int bottom = work.bottom - margin, column = 0;
        for (auto i = toasts_.rbegin(); i != toasts_.rend(); ++i) {
            auto& toast = *i;
            if (!toast->Window()) continue;
            toast->SetScale(dpi, config_.zoom, std::max(1, static_cast<int>(work.right - work.left) - 2 * margin));
            const int width = toast->Width(dpi);
            const int availableHeight = static_cast<int>(work.bottom - work.top) - 2 * margin;
            const int height = std::min(toast->Height(dpi), std::max(1, availableHeight));
            if (bottom - height < work.top + margin && bottom != work.bottom - margin) {
                ++column;
                bottom = work.bottom - margin;
            }
            const int x = ClampCoordinate(work.right - margin - width - column * (width + gap), work.left, work.right, width);
            const int y = ClampCoordinate(bottom - height, work.top, work.bottom, height);
            toast->Place(x, y, height, dpi);
            bottom = y - gap;
        }
    }
    void Notify(const NetEvent& event) {
        if (!config_.notify) return;
        if (config_.notify_sound) AlertSound(true);
        auto toast = std::make_unique<Toast>(event, window_);
        if (toast->Open(instance_, dpi_, config_.zoom)) {
            toasts_.push_back(std::move(toast));
            ArrangeToasts();
        }
    }
    void ClearNetworkNotifications() {
        MSG message{};
        while (PeekMessageW(&message, window_, NetMessage, NetMessage, PM_REMOVE))
            delete reinterpret_cast<NetEvent*>(message.lParam);
        toasts_.clear();
    }
    void Shutdown() {
        if (stopping_) return;
        stopping_ = true;
        Save();
        running_ = active_ = false;
        KillTimer(window_, SampleTimer);
        callbackWindow_.store(nullptr, std::memory_order_release);
        if (metrics_) metrics_->SetPaused(true);
        taskbar_.Close();
        metrics_.reset(); // Joins callbacks while HWND and Application are still alive.
        ClearNetworkNotifications();
        RemoveTray();
        PostQuitMessage(0);
    }
    LRESULT Handle(UINT message, WPARAM w, LPARAM l) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            tray_ = false; AddTray();
            if (config_.mode == DisplayMode::Taskbar) TrackTaskbar(true);
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            dpi_ = WindowDpi(window_);
            UpdateIcons();
            renderer_.SetWindow(window_, dpi_);
            StyleWindow(window_);
            return 0;
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PaintSession paint(window_);
            if ((!TaskbarMode() || details_) && renderer_.CanRender()) {
                const HRESULT hr = renderer_.Paint(model_.Data(), Compact(), config_.pinned, hover_, pressed_);
                renderLost_ = FAILED(hr);
                repaintPending_ = hr != S_OK;
            } else repaintPending_ = true;
            return 0;
        }
        case WM_SIZE:
            if (w == SIZE_MINIMIZED && config_.mode == DisplayMode::Taskbar && details_)
                PostMessageW(window_, TaskbarMessage, static_cast<WPARAM>(TaskbarAction::Collapse), 0);
            if (w != SIZE_MINIMIZED) {
                renderer_.Resize();
                ClampScroll();
                if (running_ && !fitting_) Fit(true);
            }
            Sampling(false);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(l);
            const RECT work = WorkArea(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
            const auto size = FittedSize(work);
            limits->ptMinTrackSize = {size.cx, size.cy};
            limits->ptMaxTrackSize = limits->ptMinTrackSize;
            return 0;
        }
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) {
                systemSuspended_ = true;
                if (metrics_) metrics_->OnSuspend();
                // Publication is serialized with OnSuspend, so this also removes
                // notifications queued immediately before the power broadcast.
                ClearNetworkNotifications();
                Sampling(false);
                return TRUE;
            }
            if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                if (metrics_) metrics_->OnResume();
                ClearNetworkNotifications();
                systemSuspended_ = false;
                model_.BreakHistory();
                Sampling(true, true); // Show Checking; HTTP remains delayed by 3 s.
                return TRUE;
            }
            break;
        case WM_SHOWWINDOW: Sampling(w != 0); break;
        case WM_TIMER: if (w == SampleTimer) Refresh(); return 0;
        case WM_DPICHANGED: {
            if (config_.mode == DisplayMode::Taskbar)
                PostMessageW(window_, TaskbarMessage, static_cast<WPARAM>(TaskbarAction::Changed), 0);
            dpi_ = HIWORD(w);
            UpdateIcons();
            renderer_.SetDpi(dpi_);
            // SetWindowPos can synchronously change DPI while Fit is already in progress.
            if (fitting_) {
                if (!dpiFitPending_) {
                    dpiFitPending_ = true;
                    PostMessageW(window_, DpiFitMessage, 0, 0);
                }
            } else Fit(false, reinterpret_cast<RECT*>(l));
            ArrangeToasts();
            Save();
            if (!IsIconic(window_)) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            TrackTaskbar();
            Fit(false);
            PositionDetails();
            ArrangeToasts();
            if (active_) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_SETTINGCHANGE:
            taskbar_.ThemeChanged();
            TrackTaskbar();
            PositionDetails();
            if (w == SPI_SETWORKAREA) { Fit(false); ArrangeToasts(); }
            if (config_.language == i18n::Preference::Auto) SetLanguage(config_.language);
            return 0;
        case WM_EXITSIZEMOVE: Fit(false); Save(); return 0;
        case WM_DWMCOMPOSITIONCHANGED: StyleWindow(window_); return 0;
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_MAXIMIZE || (w & 0xfff0) == SC_SIZE) return 0;
            break;
        case WM_MOUSEWHEEL: {
            POINT cursor{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(window_, &cursor);
            RECT client{};
            GetClientRect(window_, &client);
            if (!PtInRect(&client, cursor)) { zoomWheelDelta_ = 0; return 0; }
            if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
                zoomWheelDelta_ += GET_WHEEL_DELTA_WPARAM(w);
                const int steps = zoomWheelDelta_ / WHEEL_DELTA;
                zoomWheelDelta_ %= WHEEL_DELTA;
                if (steps) SetZoom(config_.zoom + steps * 10);
                return 0;
            }
            zoomWheelDelta_ = 0;
            const float previous = scroll_;
            scroll_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA * 66;
            ClampScroll();
            if (scroll_ != previous) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            pressed_ = Hit(l);
            if (pressed_ != Button::None) InvalidateRect(window_, nullptr, FALSE);
            else {
                GetCursorPos(&dragOrigin_);
                GetWindowRect(window_, &dragRect_);
                dragCandidate_ = true;
            }
            SetCapture(window_);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            if (dragCandidate_ && GetCapture() == window_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                const int dx = cursor.x - dragOrigin_.x, dy = cursor.y - dragOrigin_.y;
                if (dragging_ || std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) || std::abs(dy) >= GetSystemMetrics(SM_CYDRAG)) {
                    dragging_ = true;
                    SetWindowPos(window_, nullptr, dragRect_.left + dx, dragRect_.top + dy, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            } else {
                const auto hover = Hit(l);
                if (hover != hover_) { hover_ = hover; InvalidateRect(window_, nullptr, FALSE); }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto pressed = pressed_;
            const bool click = pressed != Button::None && Hit(l) == pressed;
            const bool moved = dragging_;
            pressed_ = Button::None;
            dragging_ = dragCandidate_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            if (moved) { Fit(false); Save(); }
            if (pressed != Button::None) InvalidateRect(window_, nullptr, FALSE);
            if (click) ActivateButton(pressed);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (Hit(l) == Button::None) {
                if (TaskbarMode()) ToggleDetails(); else SetCompact(!Compact());
            }
            return 0;
        case WM_MOUSELEAVE:
            zoomWheelDelta_ = 0;
            if (hover_ != Button::None) { hover_ = Button::None; InvalidateRect(window_, nullptr, FALSE); }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: {
            const bool redraw = pressed_ != Button::None;
            if (dragging_) { Fit(false); Save(); }
            dragging_ = dragCandidate_ = false;
            pressed_ = Button::None;
            if (redraw && active_) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                POINT p{};
                GetCursorPos(&p);
                ScreenToClient(window_, &p);
                const auto button = renderer_.HitTest(p.x / Scale(), p.y / Scale(), Compact(), CurrentWidth());
                SetCursor(LoadCursorW(nullptr, button == Button::None ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CONTEXTMENU: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (p.x == -1 && p.y == -1) GetCursorPos(&p);
            Menu(p);
            return 0;
        }
        case WM_COMMAND: Command(LOWORD(w)); return 0;
        case TrayMessage:
            if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT ||
                (!trayVersion4_ && LOWORD(l) == WM_LBUTTONUP)) ToggleVisibility();
            else if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP) {
                POINT p{};
                GetCursorPos(&p);
                Menu(p);
            }
            return 0;
        case NetMessage: {
            std::unique_ptr<NetEvent> event(reinterpret_cast<NetEvent*>(l));
            if (event && !stopping_ && !systemSuspended_ &&
                w == notificationState_.load(std::memory_order_acquire)) Notify(*event);
            return 0;
        }
        case TaskbarMessage:
            if (config_.mode != DisplayMode::Taskbar || stopping_) return 0;
            if (w == static_cast<WPARAM>(TaskbarAction::Toggle)) ToggleDetails();
            else if (w == static_cast<WPARAM>(TaskbarAction::Collapse)) { if (details_) ToggleDetails(); }
            else if (w == static_cast<WPARAM>(TaskbarAction::Menu)) {
                POINT p{}; GetCursorPos(&p); Menu(p);
            } else {
                if (w == static_cast<WPARAM>(TaskbarAction::SettingsChanged)) taskbar_.ThemeChanged();
                TrackTaskbar();
            }
            return 0;
        case ShowMessage: Show(); return 0;
        case ReapToastMessage:
            toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const auto& t) { return !t->Window(); }), toasts_.end());
            ArrangeToasts();
            return 0;
        case ArrangeToastMessage: ArrangeToasts(); return 0;
        case DpiFitMessage:
            dpiFitPending_ = false;
            Fit(false);
            Save();
            if (active_) InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_QUERYENDSESSION: Save(); return TRUE;
        case WM_ENDSESSION: if (w) { Save(); DestroyWindow(window_); } return 0;
        case WM_CLOSE: Save(); DestroyWindow(window_); return 0;
        case WM_DESTROY: Shutdown(); return 0;
        case WM_NCDESTROY: {
            const HWND old = window_;
            SetWindowLongPtrW(old, GWLP_USERDATA, 0);
            window_ = nullptr;
            return DefWindowProcW(old, message, w, l);
        }
        }
        return DefWindowProcW(window_, message, w, l);
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr, smallIcon_ = nullptr;
    // Class icons must outlive every main/notification window and both classes.
    HICON classIcon_ = nullptr, classSmallIcon_ = nullptr;
    std::atomic<HWND> callbackWindow_{nullptr};
    // Low bit enables reminders; every toggle advances the value so an old
    // callback cannot replay its card after reminders have been re-enabled.
    std::atomic<UINT_PTR> notificationState_{1};
    Renderer renderer_;
    TaskbarStrip taskbar_;
    Snapshot latest_;
    bool details_ = false;
    bool taskbarFallback_ = false;
    ULONGLONG taskbarRetryAt_ = 0; // Checked on SampleTimer; no extra wakeups.
    Model model_;
    Config config_;
    std::unique_ptr<Metrics> metrics_;
    std::vector<std::unique_ptr<Toast>> toasts_;
    UINT dpi_ = 96, taskbarCreated_ = 0;
    // Collection-source cache, not the displayed sample. Unknown means no
    // source has been confirmed yet; a later Unknown sample does not clear it.
    PowerTrace powerTrace_ = PowerTrace::Unknown;
    int fpsStatus_ = 0;
    bool active_ = false, running_ = false, stopping_ = false, fitting_ = false, dpiFitPending_ = false;
    bool zoomFitPending_ = false;
    bool systemSuspended_ = false;
    bool tray_ = false, trayVersion4_ = false;
    bool registeredMain_ = false, registeredToast_ = false, renderLost_ = false;
    float scroll_ = 0;
    int zoomWheelDelta_ = 0;
    bool repaintPending_ = true;
    bool dragCandidate_ = false, dragging_ = false;
    POINT dragOrigin_{};
    RECT dragRect_{};
    RECT zoomAnchor_{};
    Button hover_ = Button::None, pressed_ = Button::None;
};

struct HandleCloser { void operator()(void* handle) const { if (handle) CloseHandle(handle); } };

bool CommandHasToken(const wchar_t* commandLine, const wchar_t* token) {
    if (!commandLine || !token || !token[0]) return false;
    const std::size_t tokenLen = std::wcslen(token);
    const wchar_t* p = commandLine;
    while (*p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        const bool quoted = *p == L'"';
        if (quoted) ++p;
        const wchar_t* start = p;
        if (quoted) while (*p && *p != L'"') ++p;
        else while (*p && *p != L' ' && *p != L'\t') ++p;
        if (static_cast<std::size_t>(p - start) == tokenLen &&
            std::wmemcmp(start, token, tokenLen) == 0) return true;
        if (quoted && *p == L'"') ++p;
    }
    return false;
}

void ActivateExistingInstance() {
    HWND previous = FindWindowW(MainClass, nullptr);
    if (!previous) return;
    DWORD process = 0;
    GetWindowThreadProcessId(previous, &process);
    AllowSetForegroundWindow(process);
    PostMessageW(previous, ShowMessage, 0, 0);
}

void AllowShowFromLowerIntegrity(HWND window, UINT message) {
    using FilterFn = BOOL (WINAPI*)(HWND, UINT, DWORD, void*);
    const auto filter = reinterpret_cast<FilterFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (filter) filter(window, message, MSGFLT_ALLOW, nullptr);
}
} // namespace

void EnableDarkMode() {
    // Ordinal 135 changed its signature in Windows 10 1903. Get the real
    // build number without depending on the application's version manifest.
    using GetVersionNumbersFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    const auto getVersionNumbers = reinterpret_cast<GetVersionNumbersFn>(
        GetProcAddress(ntdll, "RtlGetNtVersionNumbers"));
    if (!getVersionNumbers) return;
    DWORD major = 0, minor = 0, build = 0;
    getVersionNumbers(&major, &minor, &build);
    build &= 0x0fffffffu; // RtlGetNtVersionNumbers includes flags in the high bits.
    if (major < 10 || build < 17763) return;

    const HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!theme) return;
    using FlushMenuThemesFn = void(WINAPI*)();
    const auto setAppMode = GetProcAddress(theme, MAKEINTRESOURCEA(135));
    const auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
        GetProcAddress(theme, MAKEINTRESOURCEA(136)));
    if (!setAppMode || !flushMenuThemes) {
        FreeLibrary(theme);
        return;
    }
    if (build >= 18362) {
        enum class PreferredAppMode : int { ForceDark = 2 };
        using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
        reinterpret_cast<SetPreferredAppModeFn>(setAppMode)(PreferredAppMode::ForceDark);
    } else {
        using AllowDarkModeForAppFn = bool(WINAPI*)(bool);
        reinterpret_cast<AllowDarkModeForAppFn>(setAppMode)(true);
    }
    flushMenuThemes();
    // Called once from wWinMain before any windows/menus. Retain the DLL for
    // the process lifetime so unloading it cannot discard the app's theme state.
}

void EnableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setContext && setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return; // Per-Monitor V2.
    using SetAwarenessFn = HRESULT(WINAPI*)(int);
    HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore) {
        const auto setAwareness = reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        const bool success = setAwareness && SUCCEEDED(setAwareness(2));
        FreeLibrary(shcore);
        if (success) return;
    }
    SetProcessDPIAware();
}

int RunPowerDiagnostics() {
    std::wstring path, error;
    const int code = WritePowerDiagnosticsFile(&path, &error);
    if (code != 0) {
        const std::wstring message = i18n::Format(Key::PowerDiagnosticsFailed, {error.empty() ? L"?" : error});
        MessageBoxW(nullptr, message.c_str(), T(Key::AppName).c_str(), MB_OK | MB_ICONERROR);
        return code;
    }
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (opened > 32) return 0;
    const std::wstring parameter = L"\"" + path + L"\"";
    const auto notepad = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"notepad.exe", parameter.c_str(), nullptr, SW_SHOWNORMAL));
    if (notepad > 32) return 0;
    MessageBoxW(nullptr, i18n::Format(Key::PowerDiagnosticsOpenFailed, {path}).c_str(),
        T(Key::AppName).c_str(), MB_OK | MB_ICONINFORMATION);
    return 0;
}

int Run(HINSTANCE instance, int showCommand, const wchar_t* commandLine) {
    // Before the single-instance mutex: this process only writes the report.
    if (CommandHasToken(commandLine, PowerDiagnosticsArg)) return RunPowerDiagnostics();
    const bool restarted = CommandHasToken(commandLine, RestartedArg);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, SingleInstanceMutex);
    if (!mutex) {
        const DWORD openError = GetLastError();
        // An elevated instance's mutex can refuse a normal launch. Still one instance.
        if (openError == ERROR_ACCESS_DENIED) ActivateExistingInstance();
        return openError == ERROR_ACCESS_DENIED ? 0 : 1;
    }
    DWORD error = GetLastError();
    if (restarted && error == ERROR_ALREADY_EXISTS) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (error == ERROR_ALREADY_EXISTS) {
            if (GetTickCount64() >= deadline) {
                if (mutex) CloseHandle(mutex);
                ActivateExistingInstance();
                return 0;
            }
            if (mutex) CloseHandle(mutex);
            mutex = nullptr;
            Sleep(50);
            mutex = CreateMutexW(nullptr, FALSE, SingleInstanceMutex);
            if (!mutex) {
                error = ERROR_ALREADY_EXISTS;
                continue;
            }
            error = GetLastError();
        }
    }
    std::unique_ptr<void, HandleCloser> uniqueInstance(mutex);
    if (error == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        return 0;
    }
    ComApartment apartment;
    if (FAILED(apartment.result)) return 1;
    Application app(instance);
    return app.Execute(showCommand, restarted);
}
} // namespace pulse::ui
