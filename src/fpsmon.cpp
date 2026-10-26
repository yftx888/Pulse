// Realtime ETW session with a unique name per monitor; never stops another instance.
// Records Present_Start only: DXGI event 42 and D3D9 event 1.
// Provider GUIDs are the public manifest ids. Event payloads are not parsed.
// DxgKrnl is not enabled: its Present event ids were not confirmed, so OpenGL
// through WGL may stay at status 3. Vulkan on Windows presents via DXGI.
//
// The session flushes about once a second, so counting arrivals between Sample
// calls jumps. Each event stores EventHeader.TimeStamp instead. StartTrace sets
// Wnode.ClientContext to 1 (QPC) and the consumer asks for raw timestamps, so
// the stamp compares directly with QueryPerformanceCounter. Sample rates the
// half-open window [now-2s, now-1s): one second long, one second late.

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
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "fpsmon.h"
#include "netprobe.h"

#include <evntrace.h>
#include <evntcons.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <limits>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#endif

#ifndef EVENT_TRACE_REAL_TIME_MODE
#define EVENT_TRACE_REAL_TIME_MODE 0x00000100
#endif
#ifndef PROCESS_TRACE_MODE_REAL_TIME
#define PROCESS_TRACE_MODE_REAL_TIME 0x00000100
#endif
#ifndef PROCESS_TRACE_MODE_RAW_TIMESTAMP
#define PROCESS_TRACE_MODE_RAW_TIMESTAMP 0x00001000
#endif
#ifndef PROCESS_TRACE_MODE_EVENT_RECORD
#define PROCESS_TRACE_MODE_EVENT_RECORD 0x10000000
#endif
#ifndef EVENT_TRACE_CONTROL_STOP
#define EVENT_TRACE_CONTROL_STOP 1
#endif
#ifndef EVENT_CONTROL_CODE_ENABLE_PROVIDER
#define EVENT_CONTROL_CODE_ENABLE_PROVIDER 1
#endif
#ifndef WNODE_FLAG_TRACED_GUID
#define WNODE_FLAG_TRACED_GUID 0x00020000
#endif
#ifndef INVALID_PROCESSTRACE_HANDLE
#define INVALID_PROCESSTRACE_HANDLE ((TRACEHANDLE)(ULONG64)-1)
#endif
#ifndef ERROR_WMI_INSTANCE_NOT_FOUND
#define ERROR_WMI_INSTANCE_NOT_FOUND 4201L
#endif
#ifndef ERROR_PRIVILEGE_NOT_HELD
#define ERROR_PRIVILEGE_NOT_HELD 1314L
#endif
#ifndef ERROR_ELEVATION_REQUIRED
#define ERROR_ELEVATION_REQUIRED 740L
#endif
#ifndef ERROR_CANCELLED
#define ERROR_CANCELLED 1223L
#endif

// Windows x64 EVENT_HEADER. A mismatch means this header cannot read the trace.
static_assert(sizeof(EVENT_HEADER) == 80, "EVENT_HEADER size");
static_assert(offsetof(EVENT_HEADER, ProcessId) == 12, "ProcessId offset");
static_assert(offsetof(EVENT_HEADER, TimeStamp) == 16, "TimeStamp offset");
static_assert(offsetof(EVENT_HEADER, ProviderId) == 24, "ProviderId offset");
static_assert(offsetof(EVENT_HEADER, EventDescriptor) == 40, "EventDescriptor offset");
static_assert(sizeof(EVENT_DESCRIPTOR) == 16, "EVENT_DESCRIPTOR size");
static_assert(offsetof(EVENT_DESCRIPTOR, Id) == 0, "event id offset");

namespace pulse {
namespace {

constexpr wchar_t kSessionPrefix[] = L"Pulse-FPS";
constexpr USHORT kDxgiPresentStart = 42;
constexpr USHORT kD3d9PresentStart = 1;
constexpr std::uint32_t kSlots = 1024;
constexpr std::uint32_t kRingInitial = 512;
constexpr std::uint32_t kRingCap = 4096;
constexpr double kWindowLagSec = 1.0;
constexpr double kWindowSpanSec = 1.0;
constexpr double kWarmupSec = 2.0;
constexpr double kRetainSec = 3.0;
// (n-1)/span over-reads a short cluster. Require the stamps to cover half the window.
constexpr double kMinSpanSec = 0.5;
constexpr double kMinIntervalSec = 0.20;
constexpr DWORD kOpenWaitMs = 3000;

// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
const GUID kDxgiProvider = {
    0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
// {783ACA0A-790E-4D7F-8451-AA850511C6B9}
const GUID kD3d9Provider = {
    0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};

static_assert((kSlots & (kSlots - 1u)) == 0u, "slot count");
static_assert(kRingInitial >= 64u && kRingInitial <= kRingCap, "ring initial");
static_assert(kRingCap <= 65536u, "ring cap");

using EnableTraceEx2Fn = ULONG (WINAPI*)(TRACEHANDLE, LPCGUID, ULONG, UCHAR, ULONGLONG, ULONGLONG, ULONG, void*);

bool IsDenied(ULONG error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD ||
           error == ERROR_ELEVATION_REQUIRED;
}

bool ValidTrace(TRACEHANDLE handle) {
    return handle != 0 && handle != INVALID_PROCESSTRACE_HANDLE;
}

bool SameGuid(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
}

double QuietNan() { return std::numeric_limits<double>::quiet_NaN(); }

std::uint64_t QpcHz() {
    static const std::uint64_t hz = []() -> std::uint64_t {
        LARGE_INTEGER freq{};
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0;
        return static_cast<std::uint64_t>(freq.QuadPart);
    }();
    return hz;
}

std::int64_t QpcNow() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double QpcSeconds(std::int64_t now, std::int64_t then) {
    const std::uint64_t hz = QpcHz();
    if (hz == 0 || now <= then) return 0.0;
    return static_cast<double>(now - then) / static_cast<double>(hz);
}

std::uint64_t SecondsToTicks(double seconds) {
    const std::uint64_t hz = QpcHz();
    if (hz == 0 || !(seconds > 0.0)) return 0;
    return static_cast<std::uint64_t>(static_cast<double>(hz) * seconds + 0.5);
}

struct TraceBuffer {
    EVENT_TRACE_PROPERTIES props;
    wchar_t logger[1024];
    wchar_t logfile[1024];
};

static_assert(offsetof(TraceBuffer, logger) >= sizeof(EVENT_TRACE_PROPERTIES), "logger room");

void Prepare(TraceBuffer& buf, const wchar_t* name, bool sized) {
    std::memset(&buf, 0, sizeof(buf));
    buf.props.Wnode.BufferSize = static_cast<ULONG>(sizeof(buf));
    buf.props.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    // 1 = QueryPerformanceCounter. Paired with PROCESS_TRACE_MODE_RAW_TIMESTAMP
    // so EVENT_HEADER.TimeStamp stays in QPC ticks instead of system time.
    buf.props.Wnode.ClientContext = 1;
    buf.props.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    buf.props.LoggerNameOffset = static_cast<ULONG>(offsetof(TraceBuffer, logger));
    buf.props.FlushTimer = 1;
    for (std::size_t i = 0; name[i] != 0 && i + 1 < 1024; ++i) buf.logger[i] = name[i];
    if (sized) {
        buf.props.LogFileNameOffset = static_cast<ULONG>(offsetof(TraceBuffer, logfile));
        buf.props.BufferSize = 64;
        buf.props.MinimumBuffers = 4;
        buf.props.MaximumBuffers = 16;
    }
}

EnableTraceEx2Fn LoadEnableTraceEx2() {
    HMODULE module = GetModuleHandleW(L"advapi32.dll");
    if (!module) module = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return nullptr;
    return reinterpret_cast<EnableTraceEx2Fn>(GetProcAddress(module, "EnableTraceEx2"));
}

// Anonymous unions use ProcessTraceMode directly. NONAMELESSUNION uses u / u2.
template <typename Log>
auto SetConsumer(Log& log, void (WINAPI* callback)(PEVENT_RECORD), int)
    -> decltype(log.ProcessTraceMode = 0u, log.EventRecordCallback = callback, void()) {
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                           PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = callback;
}

template <typename Log>
auto SetConsumer(Log& log, void (WINAPI* callback)(PEVENT_RECORD), long)
    -> decltype(log.u.ProcessTraceMode = 0u, log.u2.EventRecordCallback = callback, void()) {
    log.u.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                             PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.u2.EventRecordCallback = callback;
}

ULONG EnableOne(EnableTraceEx2Fn enable, TRACEHANDLE session, const GUID& id) {
    const ULONGLONG masks[] = {~0ULL, 0ULL};
    const UCHAR levels[] = {0xFF, 5};
    ULONG error = ERROR_INVALID_PARAMETER;
    for (UCHAR level : levels) {
        for (ULONGLONG mask : masks) {
            error = enable(session, &id, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, mask, 0ULL, 0, nullptr);
            if (error == ERROR_SUCCESS || IsDenied(error) || error != ERROR_INVALID_PARAMETER) return error;
        }
    }
    return error;
}

struct CsLock {
    explicit CsLock(CRITICAL_SECTION& cs) : cs_(cs) { EnterCriticalSection(&cs_); }
    ~CsLock() { LeaveCriticalSection(&cs_); }
    CsLock(const CsLock&) = delete;
    CsLock& operator=(const CsLock&) = delete;

    CRITICAL_SECTION& cs_;
};

// Per-process QPC timestamps. head is the next write. The newest `count`
// entries are the retained events; when full, head is also the oldest.
struct Ring {
    std::uint64_t* stamps = nullptr;
    std::uint32_t head = 0;
    std::uint32_t count = 0;
    std::uint32_t cap = 0;

    Ring() = default;
    ~Ring() { reset(); }
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    void reset() {
        delete[] stamps;
        stamps = nullptr;
        head = count = cap = 0;
    }
};

struct Span {
    std::uint32_t pid = 0;
    std::uint64_t n = 0;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

std::uint32_t IndexOf(const Ring& ring, std::uint32_t chronological) {
    std::uint32_t idx = ring.head + ring.cap - ring.count + chronological;
    while (idx >= ring.cap) idx -= ring.cap;
    return idx;
}

std::uint64_t RetainTicks() {
    static const std::uint64_t ticks = SecondsToTicks(kRetainSec);
    return ticks;
}

bool GrowRing(Ring& ring) {
    std::uint32_t ncap = ring.cap == 0 ? kRingInitial : ring.cap * 2u;
    if (ncap > kRingCap) ncap = kRingCap;
    if (ncap <= ring.cap) return false;
    std::uint64_t* next = new (std::nothrow) std::uint64_t[ncap];
    if (!next) return false;
    for (std::uint32_t i = 0; i < ring.count; ++i) next[i] = ring.stamps[IndexOf(ring, i)];
    delete[] ring.stamps;
    ring.stamps = next;
    ring.cap = ncap;
    ring.head = ring.count;
    return true;
}

void PushRing(Ring& ring, std::uint64_t ts) {
    if (ring.cap == 0 && !GrowRing(ring)) return;
    const std::uint64_t retain = RetainTicks();
    const std::uint64_t cutoff = ts > retain ? ts - retain : 0;
    while (ring.count > 0 && ring.stamps[IndexOf(ring, 0)] < cutoff) --ring.count;
    if (ring.count == ring.cap) {
        const std::uint64_t keep = RetainTicks();
        const std::uint64_t oldest = ring.stamps[ring.head];
        const bool covers = keep > 0 && ts >= oldest && (ts - oldest) >= keep;
        if (!covers && ring.cap < kRingCap) GrowRing(ring);
    }
    if (ring.stamps == nullptr || ring.cap == 0 || ring.head >= ring.cap) return;
    ring.stamps[ring.head] = ts;
    if (++ring.head == ring.cap) ring.head = 0;
    if (ring.count < ring.cap) ++ring.count;
}

void CountRing(const Ring& ring, std::uint64_t lo, std::uint64_t hi, Span& span) {
    span.n = 0;
    span.first = 0;
    span.last = 0;
    if (ring.stamps == nullptr || ring.count == 0 || ring.cap == 0 || !(lo < hi)) return;
    for (std::uint32_t i = 0; i < ring.count; ++i) {
        const std::uint64_t ts = ring.stamps[IndexOf(ring, i)];
        if (ts < lo || ts >= hi) continue;
        if (span.n == 0 || ts < span.first) span.first = ts;
        if (span.n == 0 || ts > span.last) span.last = ts;
        ++span.n;
    }
}

// n/1s when a single interval is not meaningful. Otherwise the mean interval
// inside the window, which cancels the ±1 frame the hard window edges add.
double FpsOf(const Span& span) {
    if (span.n < 2 || span.last <= span.first) return static_cast<double>(span.n);
    const std::uint64_t hz = QpcHz();
    if (hz == 0) return static_cast<double>(span.n);
    const double seconds = static_cast<double>(span.last - span.first) / static_cast<double>(hz);
    if (!(seconds >= kMinSpanSec)) return static_cast<double>(span.n);
    return static_cast<double>(span.n - 1) / seconds;
}

}  // namespace

struct FpsMonitor::Impl {
    struct Slot {
        std::uint32_t pid = 0;
        std::uint64_t newest = 0;
        Ring ring;
    };

    Impl();
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool BringUp(int& status);
    void Shutdown();
    bool NoteConsumerExit();
    void Sample(const DWORD* pids, std::size_t count, double& fps, int& status, DWORD& source,
                FpsPresent* top, int top_cap, int& top_count);
    void Add(DWORD pid, std::uint64_t ts);
    Slot* FindSlot(std::uint32_t key, bool create, std::uint64_t ts = 0);
    void Scan(std::uint64_t lo, std::uint64_t hi, const DWORD* pids, std::size_t count, DWORD root, Span& best,
              bool& found, bool& missing);
    void FillTop();
    void ClearSlots();
    void PublishOpen(unsigned long error, int phase);
    void OnRecord(const EVENT_RECORD* record);
    void Run();

    static void WINAPI OnEvent(PEVENT_RECORD record);
    static DWORD WINAPI ThreadMain(LPVOID param);

    Slot slots_[kSlots];
    // Session lifetime (trace handle, stopping_). Never taken from the callback:
    // Shutdown can hold it across CloseTrace, and CloseTrace waits for OnEvent.
    CRITICAL_SECTION lock_{};
    // Timestamp rings only. The callback holds it for one append.
    CRITICAL_SECTION ring_lock_{};
    HANDLE ready_ = nullptr;
    HANDLE thread_ = nullptr;
    TRACEHANDLE trace_ = 0;
    TRACEHANDLE session_ = 0;
    bool session_open_ = false;
    bool stopping_ = false;
    bool running_ = false;
    wchar_t logger_name_[96]{};
    unsigned long last_error_ = ERROR_SUCCESS;
    std::atomic<unsigned long> consumer_error_{ERROR_SUCCESS};
    std::atomic<int> open_phase_{0};
    std::atomic<unsigned long> open_error_{0};
    int fail_status_ = 0;
    bool have_family_ = false;
    bool window_ready_ = false;
    DWORD family_root_ = 0;
    std::int64_t session_qpc_ = 0;
    std::int64_t last_sample_qpc_ = 0;
    double last_fps_ = std::numeric_limits<double>::quiet_NaN();
    int last_status_ = 0;
    DWORD last_source_ = 0;
    FpsPresent last_top_[3]{};
    int last_top_count_ = 0;
    std::vector<Span> rows_;
};

FpsMonitor::Impl::Impl() {
    InitializeCriticalSection(&lock_);
    InitializeCriticalSection(&ring_lock_);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) last_error_ = GetLastError();
    trace_ = INVALID_PROCESSTRACE_HANDLE;
    static std::atomic<unsigned long> serial{0};
    std::swprintf(logger_name_, 96, L"%ls-%lu-%llu-%lu", kSessionPrefix,
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(GetTickCount64()),
                  serial.fetch_add(1, std::memory_order_relaxed));
}

FpsMonitor::Impl::~Impl() {
    Shutdown();
    if (ready_) CloseHandle(ready_);
    DeleteCriticalSection(&ring_lock_);
    DeleteCriticalSection(&lock_);
}

void FpsMonitor::Impl::ClearSlots() {
    CsLock guard(ring_lock_);
    for (Slot& slot : slots_) {
        slot.ring.reset();
        slot.pid = 0;
        slot.newest = 0;
    }
}

void FpsMonitor::Impl::PublishOpen(unsigned long error, int phase) {
    open_error_.store(error, std::memory_order_release);
    open_phase_.store(phase, std::memory_order_release);
    if (ready_) SetEvent(ready_);
}

void WINAPI FpsMonitor::Impl::OnEvent(PEVENT_RECORD record) {
    if (!record || !record->UserContext) return;
    static_cast<Impl*>(record->UserContext)->OnRecord(record);
}

void FpsMonitor::Impl::OnRecord(const EVENT_RECORD* record) {
    const USHORT id = record->EventHeader.EventDescriptor.Id;
    if (id != kDxgiPresentStart && id != kD3d9PresentStart) return;
    const GUID& provider = record->EventHeader.ProviderId;
    if (id == kDxgiPresentStart) {
        if (!SameGuid(provider, kDxgiProvider)) return;
    } else if (!SameGuid(provider, kD3d9Provider)) {
        return;
    }
    const LONGLONG raw = record->EventHeader.TimeStamp.QuadPart;
    if (raw <= 0) return;
    Add(record->EventHeader.ProcessId, static_cast<std::uint64_t>(raw));
}

void FpsMonitor::Impl::Add(DWORD pid, std::uint64_t ts) {
    if (pid == 0 || pid == 0xFFFFFFFFu || ts == 0) return;
    CsLock guard(ring_lock_);
    Slot* slot = FindSlot(static_cast<std::uint32_t>(pid), true, ts);
    if (!slot) return;
    if (ts > slot->newest) slot->newest = ts;
    PushRing(slot->ring, ts);
}

FpsMonitor::Impl::Slot* FpsMonitor::Impl::FindSlot(std::uint32_t key, bool create, std::uint64_t ts) {
    std::uint32_t index = (key * 0x9E3779B1u) & (kSlots - 1u);
    for (std::uint32_t n = 0; n < kSlots; ++n) {
        Slot& slot = slots_[index];
        if (slot.pid == key) return &slot;
        if (slot.pid == 0) {
            if (!create) return nullptr;
            slot.pid = key;
            return &slot;
        }
        index = (index + 1u) & (kSlots - 1u);
    }
    // With no empty slots the whole table is searched, so replacing an expired
    // entry preserves lookups. Avoid exhausting 1024 lifetime pids on long runs.
    if (create && ts > RetainTicks()) {
        for (Slot& slot : slots_) {
            if (slot.newest >= ts - RetainTicks()) continue;
            slot.pid = key;
            slot.newest = 0;
            slot.ring.head = slot.ring.count = 0;
            return &slot;
        }
    }
    return nullptr;
}

void FpsMonitor::Impl::Scan(std::uint64_t lo, std::uint64_t hi, const DWORD* pids, std::size_t count, DWORD root,
                            Span& best, bool& found, bool& missing) {
    rows_.clear();
    if (rows_.capacity() < kSlots) rows_.reserve(kSlots);
    found = false;
    missing = false;
    best = Span{};
    {
        CsLock guard(ring_lock_);
        std::uint32_t used = 0;
        for (const Slot& slot : slots_) {
            if (slot.pid == 0) continue;
            ++used;
            Span span;
            span.pid = slot.pid;
            CountRing(slot.ring, lo, hi, span);
            if (span.n > 0) rows_.push_back(span);
        }
        if (used == kSlots && pids && root != 0) {
            for (std::size_t i = 0; i < count; ++i) {
                if (pids[i] && FindSlot(static_cast<std::uint32_t>(pids[i]), false) == nullptr)
                    missing = true;
            }
        }
    }
    // Family matching and sorting use only the copied rows, outside the callback lock.
    if (!pids || root == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
        const DWORD pid = pids[i];
        if (pid == 0) continue;
        const Span* hit = nullptr;
        for (const Span& row : rows_) {
            if (row.pid == pid) {
                hit = &row;
                break;
            }
        }
        if (!hit) continue;
        // Equal counts prefer the foreground process, then the lower pid.
        const bool better = !found || hit->n > best.n ||
            (hit->n == best.n && (pid == root || (best.pid != root && pid < best.pid)));
        if (!better) continue;
        best = *hit;
        found = true;
    }
}

void FpsMonitor::Impl::FillTop() {
    std::size_t n = rows_.size();
    if (n > 3) n = 3;
    if (n > 0) {
        std::partial_sort(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(n), rows_.end(),
                          [](const Span& a, const Span& b) {
                              if (a.n != b.n) return a.n > b.n;
                              return a.pid < b.pid;
                          });
    }
    last_top_count_ = static_cast<int>(n);
    for (std::size_t i = 0; i < n; ++i) {
        last_top_[i].pid = rows_[i].pid;
        last_top_[i].frames = rows_[i].n;
    }
}

DWORD WINAPI FpsMonitor::Impl::ThreadMain(LPVOID param) {
    static_cast<Impl*>(param)->Run();
    return 0;
}

void FpsMonitor::Impl::Run() {
    EVENT_TRACE_LOGFILEW log;
    std::memset(&log, 0, sizeof(log));
    log.LoggerName = logger_name_;
    SetConsumer(log, &FpsMonitor::Impl::OnEvent, 0);
    log.Context = this;

    const TRACEHANDLE opened = OpenTraceW(&log);
    if (!ValidTrace(opened)) {
        PublishOpen(GetLastError(), 2);
        return;
    }

    EnterCriticalSection(&lock_);
    if (stopping_) {
        LeaveCriticalSection(&lock_);
        CloseTrace(opened);
        PublishOpen(ERROR_CANCELLED, 2);
        return;
    }
    trace_ = opened;
    LeaveCriticalSection(&lock_);
    PublishOpen(ERROR_SUCCESS, 1);

    TRACEHANDLE local = opened;
    consumer_error_.store(ProcessTrace(&local, 1, nullptr, nullptr), std::memory_order_release);

    EnterCriticalSection(&lock_);
    const bool owned = trace_ == opened;
    if (owned) trace_ = INVALID_PROCESSTRACE_HANDLE;
    LeaveCriticalSection(&lock_);
    if (owned) CloseTrace(opened);
}

bool FpsMonitor::Impl::BringUp(int& status) {
    status = 4;
    auto fail = [&](const wchar_t* stage, ULONG error) {
        last_error_ = error;
        status = IsDenied(error) ? 2 : 4;
        diagnostics::WriteLog(L"FPS", L"session start failed name=" + std::wstring(logger_name_) +
            L" stage=" + stage + L" error=" + std::to_wstring(error));
        return false;
    };
    if (!ready_) return fail(L"CreateEvent", last_error_);
    if (QpcHz() == 0) return fail(L"QPC", ERROR_NOT_SUPPORTED);

    TraceBuffer buf;
    Prepare(buf, logger_name_, true);
    TRACEHANDLE created = 0;
    ULONG error = StartTraceW(&created, logger_name_, &buf.props);
    if (error == ERROR_INVALID_PARAMETER) {
        Prepare(buf, logger_name_, false);
        error = StartTraceW(&created, logger_name_, &buf.props);
    }
    if (error != ERROR_SUCCESS) return fail(L"StartTrace", error);
    session_ = created;
    session_open_ = true;

    open_phase_.store(0, std::memory_order_relaxed);
    open_error_.store(0, std::memory_order_relaxed);
    consumer_error_.store(ERROR_SUCCESS, std::memory_order_relaxed);
    ResetEvent(ready_);
    EnterCriticalSection(&lock_);
    stopping_ = false;
    trace_ = INVALID_PROCESSTRACE_HANDLE;
    LeaveCriticalSection(&lock_);

    thread_ = CreateThread(nullptr, 0, &Impl::ThreadMain, this, 0, nullptr);
    if (!thread_) return fail(L"CreateThread", GetLastError());

    HANDLE waits[2] = {ready_, thread_};
    const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, kOpenWaitMs);
    if (open_phase_.load(std::memory_order_acquire) != 1) {
        const ULONG open_error = open_error_.load(std::memory_order_acquire);
        return fail(L"OpenTrace", open_error ? open_error :
            waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    }

    EnableTraceEx2Fn enable = LoadEnableTraceEx2();
    if (!enable) return fail(L"EnableTraceEx2 lookup", ERROR_PROC_NOT_FOUND);
    const ULONG dxgi = EnableOne(enable, session_, kDxgiProvider);
    if (dxgi != ERROR_SUCCESS) return fail(L"EnableTraceEx2 DXGI", dxgi);
    // D3D9 is optional. Report its error alongside the successful DXGI session.
    const ULONG d3d9 = EnableOne(enable, session_, kD3d9Provider);
    session_qpc_ = QpcNow();
    last_error_ = ERROR_SUCCESS;
    diagnostics::WriteLog(L"FPS", L"session started name=" + std::wstring(logger_name_) +
        L" error=0 dxgi=0 d3d9=" + std::to_wstring(d3d9) + L" clock=QPC window=[now-2s,now-1s)");
    return true;
}

void FpsMonitor::Impl::Shutdown() {
    EnterCriticalSection(&lock_);
    stopping_ = true;
    const TRACEHANDLE handle = trace_;
    trace_ = INVALID_PROCESSTRACE_HANDLE;
    const bool session_open = session_open_;
    const TRACEHANDLE session = session_;
    session_open_ = false;
    session_ = 0;
    LeaveCriticalSection(&lock_);

    ULONG stop_error = ERROR_SUCCESS, close_error = ERROR_SUCCESS;
    const bool had_consumer = thread_ != nullptr;
    if (session_open) {
        TraceBuffer buf;
        Prepare(buf, logger_name_, true);
        stop_error = ControlTraceW(session, logger_name_, &buf.props, EVENT_TRACE_CONTROL_STOP);
    }
    if (ValidTrace(handle)) close_error = CloseTrace(handle);
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }

    if (session_open || had_consumer) {
        diagnostics::WriteLog(L"FPS", L"session stopped name=" + std::wstring(logger_name_) +
            L" error=" + std::to_wstring(stop_error) + L" close_error=" + std::to_wstring(close_error) +
            L" consumer_error=" + std::to_wstring(consumer_error_.load(std::memory_order_acquire)));
    }
    EnterCriticalSection(&lock_);
    stopping_ = false;
    LeaveCriticalSection(&lock_);
    ClearSlots();
    running_ = false;
    have_family_ = false;
    window_ready_ = false;
    family_root_ = 0;
    session_qpc_ = 0;
    last_sample_qpc_ = 0;
    last_fps_ = QuietNan();
    last_status_ = 0;
    last_source_ = 0;
    last_top_count_ = 0;
    std::vector<Span>().swap(rows_);
}

bool FpsMonitor::Impl::NoteConsumerExit() {
    if (!running_ || !thread_) return false;
    if (WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0) return false;
    // ProcessTrace already returned. Preserve its error before cleanup.
    const ULONG error = consumer_error_.load(std::memory_order_acquire);
    last_error_ = error ? error : ERROR_WMI_INSTANCE_NOT_FOUND;
    diagnostics::WriteLog(L"FPS", L"consumer exited name=" + std::wstring(logger_name_) +
        L" error=" + std::to_wstring(last_error_));
    Shutdown();
    fail_status_ = 4;
    return true;
}

void FpsMonitor::Impl::Sample(const DWORD* pids, std::size_t count, double& fps, int& status, DWORD& source,
                              FpsPresent* top, int top_cap, int& top_count) {
    // use_ranked keeps the top list FillTop just wrote. A short second sample
    // republishes the previous window instead of clearing it.
    auto publish = [&](double fps_value, int status_value, DWORD source_value, bool use_ranked) {
        fps = fps_value;
        status = status_value;
        source = source_value;
        last_fps_ = fps_value;
        last_status_ = status_value;
        last_source_ = source_value;
        if (!use_ranked) last_top_count_ = 0;
        top_count = 0;
        if (!top || top_cap <= 0 || last_top_count_ <= 0) return;
        int n = last_top_count_;
        if (n > top_cap) n = top_cap;
        for (int i = 0; i < n; ++i) top[i] = last_top_[i];
        top_count = n;
    };

    NoteConsumerExit();
    if (!running_) {
        publish(QuietNan(), fail_status_, 0, false);
        return;
    }

    if (count > 512) count = 512;
    const DWORD root = (pids && count > 0) ? pids[0] : 0;
    const std::int64_t now = QpcNow();
    if (session_qpc_ <= 0 || QpcHz() == 0) {
        publish(QuietNan(), 4, 0, false);
        return;
    }

    // The lagged window is only complete once the session is 2 seconds old.
    if (!(QpcSeconds(now, session_qpc_) >= kWarmupSec)) {
        family_root_ = root;
        have_family_ = true;
        publish(QuietNan(), root == 0 ? 3 : 1, 0, false);
        return;
    }

    const bool switched = have_family_ && family_root_ != root;
    if (!switched && window_ready_ && QpcSeconds(now, last_sample_qpc_) < kMinIntervalSec) {
        publish(last_fps_, last_status_, last_source_, true);
        return;
    }

    const std::uint64_t lag = SecondsToTicks(kWindowLagSec);
    const std::uint64_t span_ticks = SecondsToTicks(kWindowSpanSec);
    const auto now_u = static_cast<std::uint64_t>(now);
    if (lag == 0 || span_ticks == 0 || now_u < lag + span_ticks) {
        family_root_ = root;
        have_family_ = true;
        publish(QuietNan(), root == 0 ? 3 : 1, 0, false);
        return;
    }
    const std::uint64_t hi = now_u - lag;
    const std::uint64_t lo = hi - span_ticks;

    Span best;
    bool found = false;
    bool missing = false;
    Scan(lo, hi, pids, count, root, best, found, missing);
    FillTop();
    last_sample_qpc_ = now;
    window_ready_ = true;
    family_root_ = root;
    have_family_ = true;

    if (root == 0 || !pids) {
        publish(QuietNan(), 3, 0, true);
        return;
    }
    // Foreground changed since the previous sample. The window itself is not
    // mixed (counts are per process), but the first sample of a new target
    // stays NaN, matching the previous interval rule.
    if (switched) {
        publish(QuietNan(), 1, 0, true);
        return;
    }
    if (found && best.n > 0) {
        publish(FpsOf(best), 1, static_cast<DWORD>(best.pid), true);
        return;
    }
    publish(QuietNan(), missing ? 4 : 3, 0, true);
}

FpsMonitor::FpsMonitor() : impl_(nullptr) {}

FpsMonitor::~FpsMonitor() { delete impl_; }

bool FpsMonitor::Start() {
    if (!impl_) impl_ = new Impl; // No FPS events, locks, buffers or thread while disabled.
    impl_->NoteConsumerExit();
    if (impl_->running_) return true;
    impl_->Shutdown();
    int status = 4;
    if (!impl_->BringUp(status)) {
        impl_->Shutdown();
        impl_->fail_status_ = status;
        impl_->running_ = false;
        return false;
    }
    impl_->fail_status_ = 1;
    impl_->running_ = true;
    return true;
}

void FpsMonitor::Stop() {
    delete impl_;
    impl_ = nullptr;
}

bool FpsMonitor::Active() const { return impl_ && impl_->running_; }

unsigned long FpsMonitor::LastError() const { return impl_ ? impl_->last_error_ : ERROR_SUCCESS; }

void FpsMonitor::Sample(const DWORD* pids, std::size_t count, double& fps, int& status, DWORD& source,
                        FpsPresent* top, int top_cap, int& top_count) {
    if (!impl_) {
        fps = QuietNan();
        status = 0;
        source = 0;
        top_count = 0;
        return;
    }
    impl_->Sample(pids, count, fps, status, source, top, top_cap, top_count);
}

}  // namespace pulse
