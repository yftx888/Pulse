// WinHTTP probe. Notifications use the Metrics::Start callback.
// Log file: %LOCALAPPDATA%\Pulse\pulse_net.log.
// Shutdown signals the worker and joins it. Async requests are cancelled.
// Each stage has one deadline; callback contexts live until HANDLE_CLOSING.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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
#define _WIN32_WINNT 0x0603
#endif

#include "netprobe.h"
#include "probe_status.h"

#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cwchar>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "winhttp.lib")
#endif

namespace pulse {
namespace diagnostics {
namespace {
std::atomic<TimingCallback> timing_callback{nullptr};
std::atomic<void*> timing_user{nullptr};
std::uint64_t ThreadCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}
double CounterFrequency() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? static_cast<double>(value.QuadPart) : 0.0;
    }();
    return frequency;
}
}
void SetTimingCallback(TimingCallback callback, void* user) {
    timing_user.store(user, std::memory_order_relaxed);
    timing_callback.store(callback, std::memory_order_release);
}
ScopedTiming::ScopedTiming(const wchar_t* category) noexcept {
    callback_ = timing_callback.load(std::memory_order_acquire);
    if (!callback_) return;
    user_ = timing_user.load(std::memory_order_relaxed);
    category_ = category;
    (void)CounterFrequency();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    start_ = now.QuadPart;
    cpu_ = ThreadCpuTicks();
}
ScopedTiming::~ScopedTiming() noexcept {
    if (!callback_) return;
    const std::uint64_t cpu = ThreadCpuTicks();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double frequency = CounterFrequency();
    const double wall_ms = frequency > 0 ? (now.QuadPart - start_) * 1000.0 / frequency : 0.0;
    const double cpu_ms = cpu >= cpu_ ? static_cast<double>(cpu - cpu_) / 10000.0 : 0.0;
    try { callback_(category_, wall_ms, cpu_ms, user_); } catch (...) {}
}
} // namespace diagnostics

std::wstring FormatOutageDuration(std::uint64_t seconds) {
    if (seconds >= 3600) return std::to_wstring(seconds / 3600) + L" 小时 " +
                               std::to_wstring((seconds / 60) % 60) + L" 分";
    if (seconds >= 60) return std::to_wstring(seconds / 60) + L" 分 " +
                             std::to_wstring(seconds % 60) + L" 秒";
    return std::to_wstring(seconds) + L" 秒";
}

namespace {

const wchar_t* kDefaultUrls[] = {
    L"http://www.msftconnecttest.com/connecttest.txt",
    L"http://www.gstatic.com/generate_204",
    L"http://connect.rom.miui.com/generate_204",
};

enum class Display { Checking, Online, Slow, Offline, Unusable };

constexpr ULONGLONG kRoundTimeoutMs = 2500;
constexpr ULONGLONG kConfirmIntervalMs = 1000;
constexpr ULONGLONG kOfflineIntervalMs = 1500;
constexpr ULONGLONG kNicDebounceMs = 300;
constexpr ULONGLONG kResumeDelayMs = 3000;
constexpr ULONGLONG kSuspendGapMs = 60000;
constexpr ULONGLONG kProbeLogIntervalMs = 10000;

std::int64_t ProbeClockNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ULONGLONG ProbeElapsedMs(std::int64_t start, std::int64_t end) {
    if (end <= start) return 0;
    return static_cast<ULONGLONG>((end - start) / 1000000);
}

std::wstring GetEnv(const wchar_t* name) {
    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return L"";
    std::wstring value(need, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, &value[0], need);
    if (got == 0 || got >= need) return L"";
    value.resize(got);
    return value;
}

std::wstring MakeLogPath() {
    std::wstring base = GetEnv(L"LOCALAPPDATA");
    if (base.empty()) {
        const std::wstring user = GetEnv(L"USERPROFILE");
        if (!user.empty()) base = user + L"\\AppData\\Local";
    }
    if (base.empty()) base = L".";
    return base + L"\\Pulse\\pulse_net.log";
}

void EnsureParent(const std::wstring& path) {
    const std::size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0) return;
    CreateDirectoryW(path.substr(0, pos).c_str(), nullptr);
}

bool WriteAll(HANDLE file, const void* buffer, DWORD count) {
    auto bytes = static_cast<const unsigned char*>(buffer);
    while (count) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, count, &written, nullptr) || written == 0) return false;
        bytes += written;
        count -= written;
    }
    return true;
}

std::wstring LocalTimeText(const SYSTEMTIME& time, bool date) {
    wchar_t buffer[64];
    buffer[0] = L'\0';
    if (date) {
        std::swprintf(buffer, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
                      static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                      static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                      static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond));
    } else {
        std::swprintf(buffer, 64, L"%02u:%02u", static_cast<unsigned>(time.wHour),
                      static_cast<unsigned>(time.wMinute));
    }
    return buffer;
}

std::int64_t LocalSystemTimeToUnix(const SYSTEMTIME& local) {
    FILETIME local_ft{};
    FILETIME utc_ft{};
    if (!SystemTimeToFileTime(&local, &local_ft)) return 0;
    if (!LocalFileTimeToFileTime(&local_ft, &utc_ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = utc_ft.dwLowDateTime;
    u.HighPart = utc_ft.dwHighDateTime;
    const unsigned long long epoch = 11644473600ULL;
    const unsigned long long secs = u.QuadPart / 10000000ULL;
    if (secs < epoch) return 0;
    return static_cast<std::int64_t>(secs - epoch);
}

int ClampInt(int value, int minimum, int maximum, int fallback) {
    if (value < minimum || value > maximum) return fallback;
    return value;
}

// Every outstanding callback keeps its context and this event alive. Closing
// losing requests never waits on a per-request timeout on the result path.
struct ProbeSignal {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ProbeSignal() { if (event) CloseHandle(event); }
};
struct ProbeContext {
    std::shared_ptr<ProbeSignal> signal;
    std::atomic<bool> sent{false};
    std::atomic<bool> headers{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> timed_out{false};
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<DWORD> phase{0};
    std::atomic<DWORD_PTR> error_api{0};
    std::atomic<std::int64_t> response_tick{0};
    void Fail(DWORD code) {
        error.store(code, std::memory_order_relaxed);
        timed_out.store(code == ERROR_WINHTTP_TIMEOUT, std::memory_order_relaxed);
        response_tick.store(ProbeClockNow(), std::memory_order_relaxed);
        failed.store(true, std::memory_order_release);
        SetEvent(signal->event);
    }
    std::atomic<int> refs{1};
    explicit ProbeContext(std::shared_ptr<ProbeSignal> value) : signal(std::move(value)) {}
    void Release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};

struct ProbeRequest {
    HINTERNET request = nullptr;
    HINTERNET connection = nullptr;
    ProbeContext* probe = nullptr;
    bool receiving = false;
    bool http_checked = false;
    bool http_known = false;
    DWORD http_status = 0;
    DWORD http_error = ERROR_SUCCESS;
    std::size_t url_index = 0;
    std::int64_t start_tick = 0;
    const wchar_t* step = L"create";
    DWORD setup_error = ERROR_SUCCESS;
    bool Fail(DWORD error) {
        setup_error = error;
        if (probe) probe->Fail(error);
        return false;
    }
    ~ProbeRequest() {
        if (request) WinHttpCloseHandle(request); // Cancels pending async I/O.
        if (connection) WinHttpCloseHandle(connection);
        if (probe) probe->Release(); // HANDLE_CLOSING releases the callback reference.
    }
};

void CALLBACK ProbeCallback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD info_size) {
    if (!context) return;
    diagnostics::ScopedTiming timing(L"Network callback");
    auto* probe = reinterpret_cast<ProbeContext*>(context);
    if (status != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && status != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
        probe->phase.store(status, std::memory_order_relaxed);
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        probe->sent.store(true, std::memory_order_release);
        SetEvent(probe->signal->event);
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        probe->response_tick.store(ProbeClockNow(), std::memory_order_relaxed);
        probe->headers.store(true, std::memory_order_release);
        SetEvent(probe->signal->event);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        DWORD error = ERROR_GEN_FAILURE;
        if (info && info_size >= sizeof(WINHTTP_ASYNC_RESULT)) {
            const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
            probe->error_api.store(result->dwResult, std::memory_order_relaxed);
            error = result->dwError;
        }
        probe->Fail(error);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        probe->Release();
        break;
    default:
        break;
    }
}

const wchar_t* ProbePhase(DWORD status) {
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_DETECTING_PROXY: return L"proxy";
    case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME: return L"dns";
    case WINHTTP_CALLBACK_STATUS_NAME_RESOLVED: return L"dns_done";
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER: return L"connect";
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER: return L"connected";
    case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST: return L"send";
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: return L"sent";
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: return L"headers";
    default: return L"setup";
    }
}

struct Config {
    int interval_s = 5;
    int timeout_ms = 1500;
    int fail_threshold = 3;
    int slow_ms = 500;
    int unusable_ms = 1000;
    std::vector<std::wstring> urls;
};

Config Normalize(const Settings& settings) {
    Config config;
    config.interval_s = ClampInt(settings.probe_interval_s, 1, 86400, 5);
    // Keep the append-only Settings interface; its legacy seconds value may
    // request 1 s, but no request is allowed to retain the old 4 s timeout.
    config.timeout_ms = settings.probe_timeout_s == 1 ? 1000 : 1500;
    config.fail_threshold = ClampInt(settings.fail_threshold, 1, 1000000, 3);
    config.slow_ms = ClampInt(settings.slow_ms, 1, 999, 500);
    config.unusable_ms = ClampInt(settings.unusable_ms, 2, 2147483647, 1000);
    if (config.slow_ms >= config.unusable_ms) {
        config.slow_ms = 500;
        config.unusable_ms = 1000;
    }
    if (settings.probe_urls.empty()) {
        for (const wchar_t* url : kDefaultUrls) config.urls.emplace_back(url);
    } else {
        config.urls = settings.probe_urls;
    }
    return config;
}

struct Status {
    Display display = Display::Checking;
    bool has_result = false;
    bool latest_ok = false;
    ULONGLONG latest_ms = 0;
    unsigned fail_count = 0;
    ULONGLONG first_failure_tick = 0;
    std::int64_t first_failure_unix = 0;
    SYSTEMTIME first_failure_time{};
    bool outage_active = false;
    std::int64_t outage_unix = 0;
    bool link_disconnected = false;
    OfflineReason outage_reason = OfflineReason::None;
    double outage_latency_ms = NAN;
};

class CsLock {
public:
    explicit CsLock(CRITICAL_SECTION* cs) : cs_(cs) { EnterCriticalSection(cs_); }
    ~CsLock() { Unlock(); }
    void Unlock() { if (cs_) { LeaveCriticalSection(cs_); cs_ = nullptr; } }
    CsLock(const CsLock&) = delete;
    CsLock& operator=(const CsLock&) = delete;

private:
    CRITICAL_SECTION* cs_;
};

// One append per line, including the BOM on a new file. A single WriteFile with
// FILE_APPEND_DATA keeps concurrent GUI/self-test records from overwriting.
void WriteLogLine(const std::wstring& path, const wchar_t* category, const std::wstring& message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::wstring line = L"[" + LocalTimeText(now, true) + L"] [pid=" +
        std::to_wstring(GetCurrentProcessId()) + L"] [" + category + L"] " + message + L"\r\n";
    const int count = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (count <= 0) return;
    std::string utf8(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), &utf8[0], count,
                        nullptr, nullptr);
    // Serialize GUI/self-test rotation and BOM creation. If coordination is
    // unavailable, append the complete record without attempting a rename.
    static HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Pulse.Diagnostics.Log.v1");
    const DWORD wait = mutex ? WaitForSingleObject(mutex, 100) : WAIT_FAILED;
    const bool owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    struct Release { HANDLE h; ~Release() { if (h) ReleaseMutex(h); } } release{owned ? mutex : nullptr};
    EnsureParent(path);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 1024 * 1024 && owned &&
        MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const HANDLE replacement = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (replacement != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = replacement;
            size.QuadPart = -1;
            GetFileSizeEx(file, &size);
        }
        // If reopening failed, the original handle still appends this complete
        // record to the archive. Failed renames keep appending to the live file.
    }
    if (size.QuadPart == 0) utf8.insert(0, "\xEF\xBB\xBF");
    WriteAll(file, utf8.data(), static_cast<DWORD>(utf8.size()));
    CloseHandle(file);
}

}  // namespace

void diagnostics::WriteLog(const wchar_t* category, const std::wstring& message) {
    static const std::wstring path = MakeLogPath();
    WriteLogLine(path, category, message);
}

struct NetProbe::Impl {
    Impl() {
        InitializeCriticalSection(&status_cs_);
        InitializeCriticalSection(&publication_cs_);
        cs_ready_ = true;
        log_path_ = MakeLogPath();
    }

    ~Impl() {
        Stop();
        if (wake_event_) CloseHandle(wake_event_);
        if (network_event_) CloseHandle(network_event_);
        if (cs_ready_) {
            DeleteCriticalSection(&status_cs_);
            DeleteCriticalSection(&publication_cs_);
            cs_ready_ = false;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void Start(const Settings& settings, Metrics::EventCallback cb, void* user) {
        if (thread_) return;
        cb_ = cb;
        user_ = user;
        config_ = Normalize(settings);
        EnsureParent(log_path_);

        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        unloaded_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event_ || !unloaded_event_ || !wake_event_ || !network_event_) {
            CleanupStartFailure();
            return;
        }

        proxy_mode_ = L"automatic";
        session_ = WinHttpOpen(L"Pulse/1.0.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!session_) {
            AppendLog(L"WinHttpOpen automatic failed error=" + std::to_wstring(GetLastError()) +
                      L" fallback=default");
            proxy_mode_ = L"default";
            session_ = WinHttpOpen(L"Pulse/1.0.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        }
        if (!session_) {
            AppendLog(L"WinHttpOpen failed error=" + std::to_wstring(GetLastError()));
            CleanupStartFailure();
            return;
        }
        if (!WinHttpSetOption(session_, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT, &unloaded_event_,
                              sizeof(unloaded_event_))) {
            AppendLog(L"WinHTTP unload event setup failed error=" + std::to_wstring(GetLastError()));
            CleanupStartFailure();
            return;
        }
        unload_set_ = true;

        const int full_ms = config_.timeout_ms;
        // Each internal timeout is bounded too; the outer stage/round deadlines
        // bound their sum. Do not shrink connect to 500 ms by splitting the budget:
        // that could reject otherwise usable (<1000 ms) connections.
        if (!WinHttpSetTimeouts(session_, full_ms, full_ms, full_ms, full_ms)) {
            AppendLog(L"WinHttpSetTimeouts failed error=" + std::to_wstring(GetLastError()));
            CleanupStartFailure();
            return;
        }

        thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
        if (!thread_) {
            AppendLog(L"probe CreateThread failed error=" + std::to_wstring(GetLastError()));
            CleanupStartFailure();
        }
    }

    void Stop() {
        if (stop_event_) SetEvent(stop_event_);
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        CloseSession();
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        if (unloaded_event_) {
            CloseHandle(unloaded_event_);
            unloaded_event_ = nullptr;
        }
    }

    std::wstring LogPath() const { return log_path_; }

    NetProbe::View Get() const {
        if (!cs_ready_) return view_;
        CsLock lock(&status_cs_);
        return view_;
    }

    void SetSlowThreshold(int slow_ms) {
        CsLock lock(&status_cs_);
        config_.slow_ms = ClampInt(slow_ms, 1, config_.unusable_ms - 1, 500);
        // Classification and configuration share this lock with ApplyResult.
        // Do not change failure counts, outage timestamps, logs or notifications.
        if (status_.has_result && status_.latest_ok &&
            status_.latest_ms < static_cast<ULONGLONG>(config_.unusable_ms) &&
            (status_.display == Display::Online || status_.display == Display::Slow)) {
            status_.display = status_.latest_ms >= static_cast<ULONGLONG>(config_.slow_ms)
                                  ? Display::Slow : Display::Online;
        }
        view_ = MakeView(status_);
    }

    void OnSuspend() {
        CsLock publication(&publication_cs_);
        CsLock lock(&status_cs_);
        if (suspended_) return;
        suspended_ = true;
        resume_pending_ = false;
        ++power_epoch_; // Invalidate an in-flight result before cancelling its I/O.
        std::wstring log;
        if (status_.outage_active)
            log = (L"进入睡眠，结束本次断网计时（已持续 " +
                      FormatOutageDuration((GetTickCount64() - status_.first_failure_tick) / 1000) + L"）");
        else
            log = L"进入睡眠，暂停网络探测";
        status_ = Status{};
        view_ = MakeView(status_);
        if (wake_event_) SetEvent(wake_event_);
        lock.Unlock();
        AppendLog(log);
    }

    void OnResume() {
        CsLock publication(&publication_cs_);
        CsLock lock(&status_cs_);
        // Windows may send AUTOMATIC and RESUMESUSPEND for the same resume.
        if (!suspended_ && resume_pending_) return;
        const auto log = ResetAfterResumeLocked(false);
        lock.Unlock();
        AppendLog(log);
    }

    void SetEnabled(bool enabled) {
        if (enabled_.load(std::memory_order_acquire) == enabled) return;
        CsLock publication(&publication_cs_);
        CsLock lock(&status_cs_);
        if (enabled_ == enabled) return;
        enabled_.store(enabled, std::memory_order_release);
        ++power_epoch_;
        status_ = Status{};
        view_ = MakeView(status_);
        if (!resume_pending_) resume_not_before_ = 0;
        if (wake_event_) SetEvent(wake_event_);
    }

    void RequestProbe() {
        // This handle lives until Impl destruction, including before Start and
        // after Stop. Metrics deregisters its NIC callback before destroying us.
        network_pending_.store(true, std::memory_order_release);
        if (network_event_) SetEvent(network_event_);
    }

private:
    std::wstring ResetAfterResumeLocked(bool detected_gap) {
        suspended_ = false;
        resume_pending_ = true;
        resume_not_before_ = GetTickCount64() + kResumeDelayMs;
        ++power_epoch_;
        status_ = Status{};
        view_ = MakeView(status_);
        if (wake_event_) SetEvent(wake_event_);
        return detected_gap ? L"检测到系统挂起，结束旧计时，3 秒后重新探测" : L"系统恢复，3 秒后重新探测";
    }

    void ResumeAfterGap(std::uint64_t epoch) {
        CsLock publication(&publication_cs_);
        CsLock lock(&status_cs_);
        std::wstring log;
        if (!suspended_ && epoch == power_epoch_) log = ResetAfterResumeLocked(true);
        lock.Unlock();
        if (!log.empty()) AppendLog(log);
    }

    static DWORD WINAPI ThreadProc(LPVOID param) {
        auto* self = static_cast<Impl*>(param);
        try {
            self->Run();
        } catch (...) {
            try {
                self->AppendLog(L"\u68C0\u6D4B\u51FA\u9519\uFF1A\u540E\u53F0\u63A2\u6D4B\u53D1\u751F\u5F02\u5E38\uFF0C\u76D1\u6D4B\u5DF2\u505C\u6B62");
            } catch (...) {
            }
            self->CloseSession();
        }
        return 0;
    }

    bool Stopping() const {
        return stop_event_ && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    }

    void CleanupStartFailure() {
        CloseSession();
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        if (unloaded_event_) {
            CloseHandle(unloaded_event_);
            unloaded_event_ = nullptr;
        }
    }

    void CloseSession() {
        if (!session_) return;
        HINTERNET session = session_;
        session_ = nullptr;
        const ULONGLONG start = GetTickCount64();
        AppendLog(L"WinHTTP session closing");
        const BOOL closed = WinHttpCloseHandle(session);
        const DWORD error = closed ? ERROR_SUCCESS : GetLastError();
        // WinHTTP owns this event until the last session callback has returned.
        // Do not close it after a short timeout while WinHTTP may still signal it.
        if (unload_set_ && unloaded_event_) WaitForSingleObject(unloaded_event_, INFINITE);
        unload_set_ = false;
        AppendLog(L"WinHTTP session closed error=" + std::to_wstring(error) +
                  L" wait_ms=" + std::to_wstring(GetTickCount64() - start));
    }

    void AppendLog(const std::wstring& message) const {
        WriteLogLine(log_path_, L"NET", message);
    }

    bool Interrupted() const {
        return network_pending_.load(std::memory_order_acquire) || Stopping() ||
               (wake_event_ && WaitForSingleObject(wake_event_, 0) == WAIT_OBJECT_0);
    }

    bool PendingOrSuccessful(BOOL result, ProbeRequest& item) const {
        if (result) return true;
        const DWORD error = GetLastError();
        return error == ERROR_IO_PENDING || item.Fail(error);
    }

    bool BeginRequest(const std::wstring& url, ProbeRequest& item,
                      const std::shared_ptr<ProbeSignal>& signal) {
        if (Interrupted()) return item.Fail(ERROR_OPERATION_ABORTED);
        if (url.empty() || !session_) return item.Fail(ERROR_INVALID_PARAMETER);
        item.step = L"CrackUrl";
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return item.Fail(GetLastError());
        if ((parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
            !parts.lpszHostName || !parts.dwHostNameLength ||
            parts.dwHostNameLength == static_cast<DWORD>(-1)) return item.Fail(ERROR_INVALID_PARAMETER);
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring target;
        if (parts.dwUrlPathLength && parts.dwUrlPathLength != static_cast<DWORD>(-1))
            target.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (target.empty()) target = L"/";
        if (parts.dwExtraInfoLength && parts.dwExtraInfoLength != static_cast<DWORD>(-1) &&
            parts.lpszExtraInfo) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const std::size_t fragment = target.find(L'#');
        if (fragment != std::wstring::npos) target.erase(fragment);

        item.probe = new ProbeContext(signal);
        item.step = L"Connect";
        item.connection = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
        if (!item.connection) return item.Fail(GetLastError());
        const DWORD flags = WINHTTP_FLAG_REFRESH |
            (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        item.step = L"OpenRequest";
        item.request = WinHttpOpenRequest(item.connection, L"GET", target.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!item.request) return item.Fail(GetLastError());
        // Enforce fresh request connections in WinHTTP itself, as well as the
        // HTTP header. Session/proxy selection is retained across rounds.
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_KEEP_ALIVE;
        item.step = L"SetOption";
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(item.probe);
        if (!WinHttpSetOption(item.request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
            !WinHttpSetOption(item.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            !WinHttpAddRequestHeaders(item.request, L"Connection: close\r\n", static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) return item.Fail(GetLastError());
        if (Interrupted()) return item.Fail(ERROR_OPERATION_ABORTED);

        item.probe->refs.store(2, std::memory_order_relaxed);
        const DWORD callbacks = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
            WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
            WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_STATUS_DETECTING_PROXY |
            WINHTTP_CALLBACK_STATUS_RESOLVING_NAME | WINHTTP_CALLBACK_STATUS_NAME_RESOLVED |
            WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER | WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER |
            WINHTTP_CALLBACK_STATUS_SENDING_REQUEST;
        if (WinHttpSetStatusCallback(item.request, ProbeCallback, callbacks, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
            const DWORD error = GetLastError();
            item.probe->refs.store(1, std::memory_order_relaxed);
            return item.Fail(error);
        }
        item.step = L"SendRequest";
        return PendingOrSuccessful(WinHttpSendRequest(item.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, context), item);
    }

    // Each stage has one deadline; the primary and parallel backups also share
    // a 2.5 s round deadline. A late callback can never extend either deadline.
    bool ProbeUrls(std::size_t first, std::size_t last, ULONGLONG& elapsed,
                   std::int64_t round_start, bool& timed_out, std::wstring* trace) {
        diagnostics::ScopedTiming timing(last - first > 1 ? L"Network backups (parallel)" : L"Network primary");
        const std::int64_t start = ProbeClockNow();
        const ULONGLONG round_spent = ProbeElapsedMs(round_start, start);
        if (round_spent >= kRoundTimeoutMs) {
            timed_out = true;
            if (trace) *trace += L" round_deadline error=" + std::to_wstring(ERROR_WINHTTP_TIMEOUT);
            return false;
        }
        const ULONGLONG remaining = kRoundTimeoutMs - round_spent;
        const ULONGLONG budget = remaining < static_cast<ULONGLONG>(config_.timeout_ms)
                                   ? remaining : static_cast<ULONGLONG>(config_.timeout_ms);
        auto signal = std::make_shared<ProbeSignal>();
        if (!signal->event) {
            if (trace) *trace += L" CreateEvent error=" + std::to_wstring(GetLastError());
            return false;
        }
        std::vector<std::unique_ptr<ProbeRequest>> requests;
        requests.reserve(last - first);
        // Snapshot requests before RAII cancellation, preserving the real error.
        // Formatting/file I/O stays off the callbacks and healthy result path.
        auto finish = [&](bool ok, const wchar_t* reason, DWORD fallback) {
            if (trace && (!ok || elapsed >= static_cast<ULONGLONG>(config_.unusable_ms))) {
                for (const auto& item : requests) {
                    const ProbeContext* p = item->probe;
                    const bool headers = p && p->headers.load(std::memory_order_acquire);
                    const bool failed = p && p->failed.load(std::memory_order_acquire);
                    const DWORD error = item->http_error ? item->http_error : headers ? ERROR_SUCCESS : failed ?
                        p->error.load(std::memory_order_relaxed) : item->setup_error ? item->setup_error : fallback;
                    const std::int64_t end = p && (headers || failed) ?
                        p->response_tick.load(std::memory_order_relaxed) : ProbeClockNow();
                    if (!trace->empty()) *trace += L"; ";
                    *trace += L"url#" + std::to_wstring(item->url_index + 1) + L"{" + item->step +
                        L" phase=" + (p ? ProbePhase(p->phase.load(std::memory_order_relaxed)) : L"setup") +
                        L" error=" + std::to_wstring(error) + L" api=" +
                        std::to_wstring(p ? static_cast<unsigned long long>(p->error_api.load(std::memory_order_relaxed)) : 0ULL) +
                        L" elapsed_ms=" + std::to_wstring(ProbeElapsedMs(item->start_tick, end)) +
                        L" headers=" + (headers ? L"1" : L"0") +
                        L" http=" + (item->http_known ? std::to_wstring(item->http_status) : L"?") +
                        L" result=" + reason + L"}";
                }
                if (requests.empty()) *trace += L"stage_empty error=" + std::to_wstring(fallback);
            }
            return ok;
        };
        for (std::size_t i = first; i < last; ++i) {
            if (Interrupted() || ProbeElapsedMs(start, ProbeClockNow()) >= budget) break;
            auto item = std::make_unique<ProbeRequest>();
            // Insert first so even an allocation exception leaves no live handle behind.
            requests.push_back(std::move(item));
            auto& current = *requests.back();
            current.url_index = i;
            current.start_tick = ProbeClockNow();
            BeginRequest(config_.urls[i], current, signal);
        }
        HANDLE events[] = {stop_event_, wake_event_, network_event_, signal->event};
        for (;;) {
            ResetEvent(signal->event); // Reset BEFORE inspecting flags: no lost wakeups.
            if (Interrupted()) return finish(false, L"cancelled", ERROR_OPERATION_ABORTED);
            std::int64_t winner = 0;
            bool pending = false;
            for (const auto& item : requests) {
                auto* probe = item->probe;
                if (!probe) continue;
                if (probe->headers.load(std::memory_order_acquire)) {
                    if (!item->http_checked) {
                        item->http_checked = true;
                        item->step = L"QueryHeaders";
                        DWORD size = sizeof(item->http_status);
                        // Query once on the probe thread, never in the callback.
                        item->http_known = WinHttpQueryHeaders(item->request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &item->http_status, &size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE;
                        if (!item->http_known) {
                            item->http_error = GetLastError();
                            if (!item->http_error) item->http_error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
                        } else if (!ProbeHttpStatusOk(item->http_status)) {
                            item->http_error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
                        }
                    }
                    if (item->http_known && !item->http_error) {
                        const auto tick = probe->response_tick.load(std::memory_order_relaxed);
                        if (!winner || tick < winner) winner = tick;
                    }
                } else if (!probe->failed.load(std::memory_order_acquire)) {
                    pending = true;
                    // All WinHTTP API calls on this request stay on this thread.
                    // SendRequest has returned before ReceiveResponse is started.
                    if (!item->receiving && probe->sent.load(std::memory_order_acquire)) {
                        item->receiving = true;
                        item->step = L"ReceiveResponse";
                        PendingOrSuccessful(WinHttpReceiveResponse(item->request, nullptr), *item);
                    }
                } else if (probe->timed_out.load(std::memory_order_relaxed)) {
                    // failed's acquire above makes the callback's timeout flag visible.
                    timed_out = true;
                }
            }
            if (winner) {
                elapsed = ProbeElapsedMs(start, winner);
                if (elapsed > budget) {
                    timed_out = true;
                    return finish(false, L"late_headers", ERROR_WINHTTP_TIMEOUT);
                }
                return finish(true, L"headers", ERROR_SUCCESS); // RAII cancels losing requests.
            }
            const ULONGLONG spent = ProbeElapsedMs(start, ProbeClockNow());
            if (!pending || spent >= budget) {
                if (spent >= budget) timed_out = true;
                return finish(false, spent >= budget ? L"deadline" : L"failed",
                              spent >= budget ? ERROR_WINHTTP_TIMEOUT : ERROR_GEN_FAILURE);
            }
            const DWORD waited = WaitForMultipleObjects(4, events, FALSE, static_cast<DWORD>(budget - spent));
            // The flag survives an auto-reset event wait. A late signal for an
            // already consumed notification is just a wakeup, not an HTTP failure.
            if (waited == WAIT_OBJECT_0 + 2 && !network_pending_.load(std::memory_order_acquire)) continue;
            if (waited != WAIT_OBJECT_0 + 3) {
                const DWORD error = waited == WAIT_FAILED ? GetLastError() :
                    waited == WAIT_TIMEOUT ? ERROR_WINHTTP_TIMEOUT : ERROR_OPERATION_ABORTED;
                if (waited == WAIT_TIMEOUT) timed_out = true;
                return finish(false, waited == WAIT_TIMEOUT ? L"deadline" : L"cancelled", error);
            }
        }
    }

    NetProbe::View MakeView(const Status& state) const {
        NetProbe::View view;
        switch (state.display) {
        case Display::Online:
            view.state = NetState::Online;
            break;
        case Display::Slow:
            view.state = NetState::Slow;
            break;
        case Display::Offline:
        case Display::Unusable:
            view.state = NetState::Offline;
            break;
        default:
            view.state = NetState::Checking;
            break;
        }
        if (state.has_result && state.latest_ok) view.latency_ms = static_cast<double>(state.latest_ms);
        else view.latency_ms = NAN;
        view.offline_by_latency = state.display == Display::Unusable;
        view.offline_since_unix = state.outage_active ? state.outage_unix : 0;
        view.offline_reason = state.outage_active ? state.outage_reason : OfflineReason::None;
        view.offline_latency_ms = state.outage_active ? state.outage_latency_ms : NAN;
        return view;
    }

    // Caller holds status_cs_: power transitions and event publication stay ordered.
    void PublishEvent(std::wstring notification, bool recovered, NetEvent& event) {
        if (!notification.empty() && !Stopping() && cb_) {
            event.recovered = recovered;
            event.message = std::move(notification);
            try { cb_(event, user_); } catch (...) {}
        }
    }

    bool ApplyLinkDisconnected(std::uint64_t epoch) {
        CsLock publication(&publication_cs_);
        CsLock lock(&status_cs_);
        if (!enabled_ || suspended_ || epoch != power_epoch_ ||
            network_pending_.load(std::memory_order_acquire)) return false;
        resume_pending_ = false;
        Status& state = status_;
        state.has_result = true;
        state.latest_ok = false;
        state.latest_ms = 0;
        state.display = Display::Offline;
        state.link_disconnected = true;
        state.outage_reason = OfflineReason::LinkDisconnected;
        state.outage_latency_ms = NAN;
        state.fail_count = static_cast<unsigned>(config_.fail_threshold);
        if (state.outage_active) {
            view_ = MakeView(state);
            return false; // Preserve an existing outage's start and avoid duplicate cards.
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        state.first_failure_tick = GetTickCount64();
        state.first_failure_unix = LocalSystemTimeToUnix(now);
        state.first_failure_time = now;
        state.outage_active = true;
        state.outage_unix = state.first_failure_unix;
        view_ = MakeView(state);
        NetEvent event{};
        event.reason = state.outage_reason;
        event.offline_start_unix = state.outage_unix;
        event.event_unix = state.first_failure_unix;
        PublishEvent(LocalTimeText(now, false) + L" 起网络连接已断开", false, event);
        lock.Unlock();
        AppendLog(L"网络断开：网络连接已断开");
        return true;
    }

    bool ApplyResult(bool& ok, ULONGLONG& ms, bool timed_out, std::uint64_t epoch) {
        CsLock publication(&publication_cs_);
        CsLock lock(&status_cs_);
        if (!enabled_ || suspended_ || epoch != power_epoch_ ||
            network_pending_.load(std::memory_order_acquire)) return false;
        // A local proxy/TUN response cannot recover a confirmed physical outage.
        // This query is confined to recovery attempts after physical disconnection.
        if (ok && status_.link_disconnected && detail::AllPhysicalNicsDisconnected()) {
            ok = false;
            ms = 0;
        }
        if (network_pending_.load(std::memory_order_acquire)) return false;
        resume_pending_ = false;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const ULONGLONG tick = GetTickCount64();
        std::wstring log;
        std::wstring notification;
        bool recovered = false;
        NetEvent event{};
        {
            Status& state = status_;
            state.has_result = true;
            state.latest_ok = ok;
            state.latest_ms = ms;
            if (ok && ms < static_cast<ULONGLONG>(config_.unusable_ms)) {
                if (state.outage_active) {
                    const ULONGLONG duration = tick - state.first_failure_tick;
                    event.reason = state.outage_reason;
                    event.latency_ms = state.outage_latency_ms;
                    event.offline_start_unix = state.outage_unix;
                    event.event_unix = LocalSystemTimeToUnix(now);
                    event.outage_duration_s = duration / 1000;
                    log = L"\u7F51\u7EDC\u5DF2\u6062\u590D\uFF0C\u65AD\u7F51\u65F6\u957F " + FormatOutageDuration(duration / 1000);
                    notification = L"\u7F51\u7EDC\u5DF2\u6062\u590D \u00B7 " + LocalTimeText(now, false) +
                                   L" \u00B7 \u672C\u6B21\u65AD\u7F51 " + FormatOutageDuration(duration / 1000);
                    recovered = true;
                    state.outage_active = false;
                } else if (state.fail_count > 0) {
                    log = L"\u77ED\u6682\u6CE2\u52A8\uFF08\u5931\u8D25 " + std::to_wstring(state.fail_count) +
                          L" \u6B21\uFF09\u540E\u6062\u590D";
                }
                state.fail_count = 0;
                state.first_failure_tick = 0;
                state.first_failure_unix = 0;
                state.link_disconnected = false;
                state.display = ms >= static_cast<ULONGLONG>(config_.slow_ms) ? Display::Slow
                                                                               : Display::Online;
            } else {
                if (state.fail_count < 0xFFFFFFFFu) ++state.fail_count;
                if (state.fail_count == 1) {
                    state.first_failure_tick = tick;
                    state.first_failure_unix = LocalSystemTimeToUnix(now);
                    state.first_failure_time = now;
                }
                if (state.fail_count == static_cast<unsigned>(config_.fail_threshold)) {
                    state.outage_active = true;
                    state.outage_unix = state.first_failure_unix;
                    state.outage_reason = state.link_disconnected ? OfflineReason::LinkDisconnected :
                        ok ? OfflineReason::HighLatency : timed_out ? OfflineReason::Timeout : OfflineReason::NoResponse;
                    state.outage_latency_ms = ok ? static_cast<double>(ms) : NAN;
                    event.reason = state.outage_reason;
                    event.latency_ms = state.outage_latency_ms;
                    event.offline_start_unix = state.outage_unix;
                    event.event_unix = LocalSystemTimeToUnix(now);
                    notification = LocalTimeText(state.first_failure_time, false) + L" 起无法访问网络";
                    if (ok) {
                        log = L"网络断开：延迟过高（" + std::to_wstring(ms) + L" ms）";
                        notification += L"（延迟 " + std::to_wstring(ms) + L" ms）";
                    } else {
                        log = timed_out ? L"网络断开：连接超时" : L"网络断开：无响应";
                        if (timed_out) notification += L"（连接超时）";
                    }
                }
                if (state.fail_count >= static_cast<unsigned>(config_.fail_threshold)) {
                    state.display = ok && !state.link_disconnected ? Display::Unusable : Display::Offline;
                } else if (ok) {
                    state.display = Display::Slow;
                }
            }
            view_ = MakeView(state);
        }
        // Serialize publication with OnSuspend/OnResume: after either returns,
        // the UI can safely drain every notification from the previous epoch.
        PublishEvent(std::move(notification), recovered, event);
        lock.Unlock();
        if (!log.empty()) AppendLog(log);
        return true;
    }

    void LogProbeRound(ULONGLONG round, std::int64_t start, bool ok, ULONGLONG ms,
                       bool timeout, bool parallel, std::uint64_t epoch,
                       const wchar_t* result, const std::wstring& trace) {
        const ULONGLONG tick = GetTickCount64();
        unsigned failures = 0;
        bool outage = false, suspended = false, physical_offline = false;
        std::uint64_t active_epoch = 0;
        {
            CsLock lock(&status_cs_);
            failures = status_.fail_count;
            outage = status_.outage_active;
            physical_offline = status_.link_disconnected;
            suspended = suspended_;
            active_epoch = power_epoch_;
        }
        // This also limits diagnostics during a storm of NIC cancellations.
        if (last_probe_log_tick_ && tick - last_probe_log_tick_ < kProbeLogIntervalMs &&
            (outage || std::wcscmp(result, L"applied") != 0)) return;
        last_probe_log_tick_ = tick;
        AppendLog(L"probe round=" + std::to_wstring(round) + L" result=" + result +
            L" ok=" + (ok ? L"1" : L"0") + L" latency_ms=" + std::to_wstring(ms) +
            L" round_ms=" + std::to_wstring(ProbeElapsedMs(start, ProbeClockNow())) +
            L" timeout=" + (timeout ? L"1" : L"0") + L" parallel=" + (parallel ? L"1" : L"0") +
            L" fail_count=" + std::to_wstring(failures) + L" offline=" + (outage ? L"1" : L"0") +
            L" physical_offline=" + (physical_offline ? L"1" : L"0") +
            L" proxy=" + proxy_mode_ + L" nic_pending=" +
            (network_pending_.load(std::memory_order_acquire) ? L"1" : L"0") +
            L" suspended=" + (suspended ? L"1" : L"0") + L" epoch=" + std::to_wstring(epoch) +
            L"/" + std::to_wstring(active_epoch) + L" cancelled_total=" + std::to_wstring(cancelled_rounds_) +
            L" since_success_ms=" + (last_success_tick_ ? std::to_wstring(tick - last_success_tick_) : L"unknown") +
            L" [" + trace + L"]");
    }

    void Run() {
        std::wstring targets;
        for (std::size_t i = 0; i < config_.urls.size(); ++i) {
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.dwHostNameLength = static_cast<DWORD>(-1);
            const bool valid = WinHttpCrackUrl(config_.urls[i].c_str(), 0, 0, &parts) != FALSE;
            targets += L" url#" + std::to_wstring(i + 1) + L"=" +
                (valid && parts.lpszHostName ? std::wstring(parts.lpszHostName, parts.dwHostNameLength) : L"invalid");
        }
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy_config{};
        std::wstring proxy_settings;
        if (WinHttpGetIEProxyConfigForCurrentUser(&proxy_config)) {
            proxy_settings = L" user_auto_detect=" + std::to_wstring(proxy_config.fAutoDetect ? 1 : 0) +
                L" user_pac=" + std::to_wstring(proxy_config.lpszAutoConfigUrl && *proxy_config.lpszAutoConfigUrl ? 1 : 0) +
                L" user_manual_proxy=" + std::to_wstring(proxy_config.lpszProxy && *proxy_config.lpszProxy ? 1 : 0);
        } else {
            proxy_settings = L" user_proxy_config_error=" + std::to_wstring(GetLastError());
        }
        if (proxy_config.lpszAutoConfigUrl) GlobalFree(proxy_config.lpszAutoConfigUrl);
        if (proxy_config.lpszProxy) GlobalFree(proxy_config.lpszProxy);
        if (proxy_config.lpszProxyBypass) GlobalFree(proxy_config.lpszProxyBypass);
        AppendLog(L"启动监测 proxy=" + std::wstring(proxy_mode_) + proxy_settings +
            L" timeout_ms=" + std::to_wstring(config_.timeout_ms) + L" round_limit_ms=" +
            std::to_wstring(kRoundTimeoutMs) + L" interval_s=" + std::to_wstring(config_.interval_s) +
            L" fail_threshold=" + std::to_wstring(config_.fail_threshold) + targets);
        ULONGLONG round = 0;
        std::uint64_t epoch = 0;
        ULONGLONG next_due = 0, last_start = 0, link_check_due = 0;
        bool link_was_disconnected = false;
        bool race_next = false;
        const ULONGLONG normal_interval = static_cast<ULONGLONG>(config_.interval_s) * 1000ULL;
        // Respect custom long intervals as well as the default 5 s cadence.
        const ULONGLONG gap_limit = normal_interval * 2 > kSuspendGapMs
            ? normal_interval * 2 : kSuspendGapMs;
        HANDLE events[] = {stop_event_, wake_event_, network_event_};
        while (!Stopping()) {
            // The flag also survives NIC events consumed inside an HTTP wait.
            const bool network_changed = network_pending_.exchange(false, std::memory_order_acq_rel);
            bool suspended = false, resuming = false, fast = false, offline = false, physical = false, enabled = false;
            {
                CsLock lock(&status_cs_);
                // Reset under the same lock as the power handlers, before
                // taking a snapshot, so a suspend/resume wakeup cannot be lost.
                ResetEvent(wake_event_);
                if (epoch != power_epoch_) {
                    epoch = power_epoch_;
                    next_due = resume_not_before_;
                    last_start = 0;
                    race_next = enabled_ && !resume_pending_;
                    link_check_due = 0;
                    link_was_disconnected = false;
                }
                suspended = suspended_;
                enabled = enabled_;
                physical = status_.link_disconnected;
                resuming = resume_pending_;
                fast = status_.fail_count != 0;
                offline = status_.outage_active;
            }
            if (!enabled) {
                const DWORD waited = WaitForMultipleObjects(3, events, FALSE, INFINITE);
                if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
                continue;
            }
            ULONGLONG now = GetTickCount64();
            if (!suspended && last_start && now - last_start > gap_limit) {
                ResumeAfterGap(epoch);
                continue;
            }
            if (network_changed && !suspended && !resuming) {
                race_next = true;
                link_was_disconnected = detail::AllPhysicalNicsDisconnected();
                link_check_due = GetTickCount64() + kNicDebounceMs;
                // Coalesce IPv4/IPv6 notification bursts; never start two rounds
                // less than 1 s apart. Changes do not reset failure counts.
                const ULONGLONG earliest = last_start && now - last_start < kConfirmIntervalMs
                    ? last_start + kConfirmIntervalMs : now;
                if (next_due > earliest) next_due = earliest;
                now = GetTickCount64();
            }
            if (!suspended && link_check_due && now >= link_check_due) {
                link_check_due = 0;
                const bool disconnected = detail::AllPhysicalNicsDisconnected();
                if (disconnected) {
                    if (link_was_disconnected) {
                        if (ApplyLinkDisconnected(epoch)) {
                            // The outage is published; resume HTTP at the existing
                            // offline cadence without three confirmations.
                            next_due = GetTickCount64() + 15000;
                        }
                    } else {
                        // Some drivers publish the IP notification before updating
                        // media state. Confirm that newly observed loss once more.
                        link_check_due = GetTickCount64() + kNicDebounceMs;
                    }
                }
                link_was_disconnected = disconnected;
                now = GetTickCount64();
            }
            if (suspended || link_check_due || now < next_due) {
                // A one-shot debounce shares this worker's existing event wait;
                // it does not add a timer, a thread or healthy-network polling.
                const ULONGLONG due = link_check_due ? link_check_due : next_due;
                const DWORD delay = suspended ? INFINITE :
                    due > now ? static_cast<DWORD>(due - now) : 0;
                const DWORD waited = WaitForMultipleObjects(3, events, FALSE, delay);
                if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
                continue;
            }

            // No HTTP while all physical links are down. NIC notifications wake
            // this wait immediately; 15 s also covers missed driver notifications.
            if ((physical || fast || !last_start) && detail::AllPhysicalNicsDisconnected()) {
                ApplyLinkDisconnected(epoch);
                last_start = now;
                next_due = now + 15000;
                continue;
            }
            last_start = now;
            const std::int64_t round_start = ProbeClockNow();
            ++round;
            const bool trace_round = !offline || !last_probe_log_tick_ ||
                now - last_probe_log_tick_ >= kProbeLogIntervalMs;
            std::wstring trace;
            std::wstring* trace_out = trace_round ? &trace : nullptr;
            ULONGLONG ms = 0;
            bool timed_out = false;
            // Once unavailable or after a NIC change, race all addresses within
            // one 1.5 s deadline. Steady healthy operation still uses only the
            // primary (and falls back only on failure), preserving its power use.
            const bool parallel = fast || race_next;
            race_next = false;
            bool ok = !config_.urls.empty() &&
                ProbeUrls(0, parallel ? config_.urls.size() : 1, ms, round_start, timed_out, trace_out);
            if (!parallel && !ok && !Interrupted() && config_.urls.size() > 1)
                ok = ProbeUrls(1, config_.urls.size(), ms, round_start, timed_out, trace_out);
            if (Stopping()) break;
            // Includes sleep that occurred inside an async WinHTTP wait, even
            // when WM_POWERBROADCAST was missed. Never apply that stale result.
            if (GetTickCount64() - last_start > kSuspendGapMs) {
                LogProbeRound(round, round_start, ok, ms, timed_out, parallel, epoch,
                              L"long_round_or_sleep", trace);
                ResumeAfterGap(epoch);
                continue;
            }
            if (Interrupted()) {
                ++cancelled_rounds_;
                if (trace_round) LogProbeRound(round, round_start, ok, ms, timed_out, parallel, epoch,
                                               L"cancelled", trace);
                continue;
            }
            if (!ok) ms = 0;
            const bool applied = ApplyResult(ok, ms, timed_out, epoch);
            if (!applied) ++cancelled_rounds_;
            if (applied && ok && ms < static_cast<ULONGLONG>(config_.unusable_ms))
                last_success_tick_ = GetTickCount64();
            else if (trace_round)
                LogProbeRound(round, round_start, ok, ms, timed_out, parallel, epoch,
                              applied ? L"applied" : L"invalidated", trace);
            {
                CsLock lock(&status_cs_);
                const ULONGLONG interval = status_.outage_active ?
                    (status_.link_disconnected ? 15000ULL :
                     GetTickCount64() - status_.first_failure_tick >= 120000 ? 3000ULL : kOfflineIntervalMs) :
                    status_.fail_count ? kConfirmIntervalMs :
                    static_cast<ULONGLONG>(config_.interval_s) * 1000ULL;
                // Cadence is measured start-to-start. If a round consumes its
                // interval, retry immediately instead of adding another delay.
                next_due = last_start + interval;
            }
        }
        AppendLog(L"退出监测");
        CloseSession();
    }

    Config config_{};
    Status status_{};
    NetProbe::View view_{};
    mutable CRITICAL_SECTION status_cs_{};
    CRITICAL_SECTION publication_cs_{}; // Orders transitions/logs without blocking Get on disk.
    std::atomic<bool> enabled_{true};
    bool cs_ready_ = false;
    std::wstring log_path_;
    Metrics::EventCallback cb_ = nullptr;
    void* user_ = nullptr;
    // Unlike stop_event_, these remain valid through Start/Stop and NIC callbacks.
    HANDLE wake_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE network_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> network_pending_{false};
    bool suspended_ = false, resume_pending_ = false; // Protected by status_cs_.
    std::uint64_t power_epoch_ = 1;
    ULONGLONG resume_not_before_ = 0;
    HANDLE stop_event_ = nullptr;
    HANDLE unloaded_event_ = nullptr;
    HANDLE thread_ = nullptr;
    HINTERNET session_ = nullptr;
    bool unload_set_ = false;
    const wchar_t* proxy_mode_ = L"automatic";
    ULONGLONG last_probe_log_tick_ = 0, last_success_tick_ = 0, cancelled_rounds_ = 0;
};

NetProbe::NetProbe() : impl_(new Impl) {}

NetProbe::~NetProbe() { delete impl_; }

void NetProbe::Start(const Settings& settings, Metrics::EventCallback cb, void* user) {
    impl_->Start(settings, cb, user);
}

void NetProbe::Stop() { impl_->Stop(); }

std::wstring NetProbe::LogPath() const { return impl_->LogPath(); }

NetProbe::View NetProbe::Get() const { return impl_->Get(); }

void NetProbe::SetSlowThreshold(int slow_ms) { impl_->SetSlowThreshold(slow_ms); }

void NetProbe::OnSuspend() { impl_->OnSuspend(); }

void NetProbe::OnResume() { impl_->OnResume(); }

void NetProbe::SetEnabled(bool enabled) { impl_->SetEnabled(enabled); }

void NetProbe::RequestProbe() { impl_->RequestProbe(); }

}  // namespace pulse
