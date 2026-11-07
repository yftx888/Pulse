#include "netprobe_mac.h"
#include "mac_strings.h"
#include "../src/probe_status.h"
#import <Network/Network.h>
#import <CFNetwork/CFNetwork.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pulse { namespace mac { namespace probe_detail {
using Clock = std::chrono::steady_clock;
std::atomic<bool> require_proxy{false};
struct Row {
    std::wstring url, error, remote_address, remote_port;
    int proxy = -1; // Unknown unless metrics arrived before the round was published.
    Clock::time_point start;
    double milliseconds = NAN;
    long status = 0;
    bool done = false, ok = false, timeout = false, proxy_rejected = false;
};
std::wstring LocalTime(std::int64_t unix_time) {
    const auto value = static_cast<std::time_t>(unix_time);
    std::tm local{};
    std::array<char, 32> text{};
    if (!localtime_r(&value, &local) || !std::strftime(text.data(), text.size(), "%H:%M:%S", &local)) return L"--";
    return FromUtf8(text.data());
}
std::wstring NetworkLogPath() {
    return ToWide([NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/Pulse/pulse_net.log"]);
}
// Write only lifecycle and outage transitions, never every successful probe.
// Keep one 1 MiB archive, matching the Windows diagnostics retention size.
void AppendNetworkLog(const std::wstring& message) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    @autoreleasepool {
        NSString* path=ToNSString(NetworkLogPath());
        NSError* error=nil;
        if (![NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent
            withIntermediateDirectories:YES attributes:nil error:&error]) {
            std::fprintf(stderr,"Pulse: network log: %s\n",error.localizedDescription.UTF8String);
            return;
        }
        const int flags=O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW;
        int fd=open(path.fileSystemRepresentation,flags,0600);
        if (fd<0) { std::fprintf(stderr,"Pulse: network log: %s\n",std::strerror(errno)); return; }
        struct stat info{};
        if (fstat(fd,&info)!=0 || !S_ISREG(info.st_mode)) { close(fd); return; }
        if (info.st_size>1024*1024) {
            NSString* archive=[path stringByAppendingString:@".1"];
            if (rename(path.fileSystemRepresentation,archive.fileSystemRepresentation)==0) {
                const int replacement=open(path.fileSystemRepresentation,flags,0600);
                if (replacement>=0) { close(fd); fd=replacement; }
            }
        }
        const auto now=std::time(nullptr);
        std::tm local{}; char timestamp[32]{};
        localtime_r(&now,&local); std::strftime(timestamp,sizeof(timestamp),"%Y-%m-%d %H:%M:%S",&local);
        const std::string line=std::string("[")+timestamp+"] [pid="+std::to_string(getpid())+"] [NET] "+Utf8(message)+"\n";
        std::size_t offset=0;
        while (offset<line.size()) {
            const auto count=write(fd,line.data()+offset,line.size()-offset);
            if (count<0 && errno==EINTR) continue;
            if (count<=0) { std::fprintf(stderr,"Pulse: network log write failed\n"); break; }
            offset+=static_cast<std::size_t>(count);
        }
        close(fd);
    }
}
std::wstring Duration(std::uint64_t seconds) {
    if (seconds >= 3600) return std::to_wstring(seconds / 3600) + L" 小时 " + std::to_wstring(seconds / 60 % 60) + L" 分";
    if (seconds >= 60) return std::to_wstring(seconds / 60) + L" 分 " + std::to_wstring(seconds % 60) + L" 秒";
    return std::to_wstring(seconds) + L" 秒";
}
struct Round {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Row> rows;
    bool cancelled = false;
    const bool collect_metrics;
    explicit Round(bool diagnostics = false) : collect_metrics(diagnostics) {}
};
Settings Normalize(Settings s) {
    s.probe_interval_s = std::clamp(s.probe_interval_s, 1, 3600);
    s.probe_timeout_s = std::clamp(s.probe_timeout_s, 1, 60);
    s.fail_threshold = std::clamp(s.fail_threshold, 1, 100);
    s.unusable_ms = std::clamp(s.unusable_ms, 2, 60000);
    s.slow_ms = std::clamp(s.slow_ms, 1, s.unusable_ms - 1);
    if (s.probe_urls.empty()) s.probe_urls = {
        L"http://www.msftconnecttest.com/connecttest.txt",
        L"http://www.gstatic.com/generate_204",
        L"http://connect.rom.miui.com/generate_204",
        L"http://captive.apple.com/hotspot-detect.html"};
    return s;
}
void Cancel(const std::shared_ptr<Round>& r) {
    if (!r) return;
    std::lock_guard<std::mutex> lock(r->mutex);
    r->cancelled = true;
    r->changed.notify_all();
}
void Complete(const std::shared_ptr<Round>& r, std::size_t index, long status,
              NSString* error, bool timeout) {
    std::lock_guard<std::mutex> lock(r->mutex);
    if (r->cancelled || index >= r->rows.size() || r->rows[index].done) return;
    Row& row = r->rows[index];
    row.done = true;
    row.status = status;
    row.error = ToWide(error);
    row.timeout = timeout;
    row.ok = !error && ProbeHttpStatusOk(static_cast<unsigned long>(status));
    row.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - row.start).count();
    r->changed.notify_all();
}
} } }

// Session callbacks own only Round, never Metrics/NetProbe/Impl or stack addresses.
@interface PulseProbeDelegate : NSObject <NSURLSessionDataDelegate> {
    std::shared_ptr<pulse::mac::probe_detail::Round> _round;
}
- (instancetype)initWithRound:(std::shared_ptr<pulse::mac::probe_detail::Round>)round;
@end
@implementation PulseProbeDelegate
- (instancetype)initWithRound:(std::shared_ptr<pulse::mac::probe_detail::Round>)round {
    self = [super init];
    if (self) _round = std::move(round);
    return self;
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    const long status = [response isKindOfClass:[NSHTTPURLResponse class]] ?
        static_cast<long>(((NSHTTPURLResponse*)response).statusCode) : 0;
    pulse::mac::probe_detail::Complete(_round, static_cast<std::size_t>(task.taskDescription.integerValue),
        status, nil, false);
    // Measure headers, not arbitrary response body length. Completion cancellation is expected.
    completionHandler(NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest* _Nullable))completionHandler {
    (void)session; (void)request;
    pulse::mac::probe_detail::Complete(_round, static_cast<std::size_t>(task.taskDescription.integerValue),
        static_cast<long>(response.statusCode), @"HTTP 重定向不算成功", false);
    completionHandler(nil);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    didFinishCollectingMetrics:(NSURLSessionTaskMetrics*)metrics {
    (void)session;
    // Only --net records metrics. Normal probes never lock/wait for this callback.
    if (!_round->collect_metrics) return;
    std::lock_guard<std::mutex> lock(_round->mutex);
    const auto index = static_cast<std::size_t>(task.taskDescription.integerValue);
    if (_round->cancelled || index >= _round->rows.size()) return;
    NSURLSessionTaskTransactionMetrics* last = metrics.transactionMetrics.lastObject;
    if (!last) return;
    auto& row = _round->rows[index];
    row.proxy = last.proxyConnection ? 1 : 0;
    if (@available(macOS 13.0, *)) {
        row.remote_address = pulse::mac::ToWide(last.remoteAddress);
        row.remote_port = pulse::mac::ToWide(last.remotePort.stringValue);
    }
    _round->changed.notify_all();
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
    (void)session;
    if (error) pulse::mac::probe_detail::Complete(_round,
        static_cast<std::size_t>(task.taskDescription.integerValue), 0,
        [NSString stringWithFormat:@"%@ (%@:%ld)", error.localizedDescription,
            error.domain, static_cast<long>(error.code)], error.code == NSURLErrorTimedOut);
}
@end

namespace pulse { namespace mac { namespace probe_detail {
struct ProxyEndpoint {
    std::wstring host;
    int port = 0;
};
struct Result {
    bool ok = false, timeout = false, cancelled = false;
    bool require_proxy = false, proxy_rejected = false;
    double ms = NAN;
    ProxyEndpoint http_proxy, https_proxy;
    std::vector<Row> rows;
};
bool ReadProxyEndpoint(NSDictionary* settings, CFStringRef enable_key, CFStringRef host_key,
                       CFStringRef port_key, ProxyEndpoint& endpoint) {
    id enabled = settings[(__bridge NSString*)enable_key];
    id host = settings[(__bridge NSString*)host_key];
    id port = settings[(__bridge NSString*)port_key];
    if (![enabled isKindOfClass:[NSNumber class]] || ![enabled boolValue] ||
        ![host isKindOfClass:[NSString class]] || ![port isKindOfClass:[NSNumber class]]) return false;
    NSString* clean_host = [host stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    const double number = [port doubleValue];
    if (!clean_host.length || !std::isfinite(number) || number < 1 || number > 65535 || std::floor(number) != number)
        return false;
    endpoint.host = ToWide(clean_host);
    endpoint.port = static_cast<int>(number);
    return !endpoint.host.empty();
}
NSDictionary* RequiredProxyDictionary(const Settings& settings, Result& result) {
    // Copy once per round; the explicit dictionary never inherits PAC, discovery or bypass lists.
    NSDictionary* system = CFBridgingRelease(CFNetworkCopySystemProxySettings());
    if (!system) return nil;
    bool http = false, https = false;
    for (const auto& text : settings.probe_urls) {
        NSString* scheme = [NSURL URLWithString:ToNSString(text)].scheme.lowercaseString;
        http = http || [scheme isEqualToString:@"http"];
        https = https || [scheme isEqualToString:@"https"];
    }
    ProxyEndpoint http_proxy, https_proxy;
    if ((http && !ReadProxyEndpoint(system, kCFNetworkProxiesHTTPEnable, kCFNetworkProxiesHTTPProxy,
                                   kCFNetworkProxiesHTTPPort, http_proxy)) ||
        (https && !ReadProxyEndpoint(system, kCFNetworkProxiesHTTPSEnable, kCFNetworkProxiesHTTPSProxy,
                                    kCFNetworkProxiesHTTPSPort, https_proxy))) return nil;
    NSMutableDictionary* explicit_proxies = [NSMutableDictionary dictionary];
    if (http) {
        explicit_proxies[(__bridge NSString*)kCFNetworkProxiesHTTPEnable] = @1;
        explicit_proxies[(__bridge NSString*)kCFNetworkProxiesHTTPProxy] = ToNSString(http_proxy.host);
        explicit_proxies[(__bridge NSString*)kCFNetworkProxiesHTTPPort] = @(http_proxy.port);
    }
    if (https) {
        explicit_proxies[(__bridge NSString*)kCFNetworkProxiesHTTPSEnable] = @1;
        explicit_proxies[(__bridge NSString*)kCFNetworkProxiesHTTPSProxy] = ToNSString(https_proxy.host);
        explicit_proxies[(__bridge NSString*)kCFNetworkProxiesHTTPSPort] = @(https_proxy.port);
    }
    result.http_proxy = std::move(http_proxy);
    result.https_proxy = std::move(https_proxy);
    return explicit_proxies;
}
Result Run(const Settings& settings, const std::shared_ptr<Round>& round, bool all) {
    @autoreleasepool {
        Result result;
        result.require_proxy = ProbeRequireProxy();
        {
            std::lock_guard<std::mutex> lock(round->mutex);
            round->rows.resize(settings.probe_urls.size());
            for (std::size_t i = 0; i < round->rows.size(); ++i) {
                round->rows[i].url = settings.probe_urls[i];
                round->rows[i].start = Clock::now();
            }
        }
        NSDictionary* explicit_proxies = nil;
        if (result.require_proxy) {
            explicit_proxies = RequiredProxyDictionary(settings, result);
            if (!explicit_proxies) {
                std::lock_guard<std::mutex> lock(round->mutex);
                result.proxy_rejected = true;
                result.cancelled = round->cancelled;
                for (Row& row : round->rows) {
                    row.done = true;
                    row.proxy_rejected = true;
                    row.milliseconds = 0;
                    row.error = L"require-proxy：系统未配置可用的 HTTP(S) 代理，未发出请求";
                }
                result.rows = round->rows;
                round->cancelled = true;
                return result; // No configuration, session or request has been created.
            }
        }
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        if (result.require_proxy) config.connectionProxyDictionary = explicit_proxies;
        config.URLCache = nil;
        config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        config.timeoutIntervalForRequest = settings.probe_timeout_s;
        config.timeoutIntervalForResource = settings.probe_timeout_s;
        config.HTTPCookieStorage = nil;
        config.HTTPShouldSetCookies = NO;
        config.waitsForConnectivity = NO;
        PulseProbeDelegate* delegate = [[PulseProbeDelegate alloc] initWithRound:round];
        NSOperationQueue* queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
        const auto deadline = Clock::now() + std::chrono::seconds(settings.probe_timeout_s);
        for (std::size_t i = 0; i < settings.probe_urls.size(); ++i) {
            { std::lock_guard<std::mutex> lock(round->mutex); if (round->cancelled) break; }
            NSURL* url = [NSURL URLWithString:ToNSString(settings.probe_urls[i])];
            NSString* scheme = url.scheme.lowercaseString;
            if (!url.host.length || (![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"https"])) {
                Complete(round, i, 0, @"无效 HTTP(S) 地址", false);
                continue;
            }
            NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
            request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            request.timeoutInterval = settings.probe_timeout_s;
            [request setValue:@"no-cache" forHTTPHeaderField:@"Cache-Control"];
            [request setValue:@"close" forHTTPHeaderField:@"Connection"];
            NSURLSessionDataTask* task = [session dataTaskWithRequest:request];
            task.taskDescription = [NSString stringWithFormat:@"%zu", i];
            [task resume];
        }
        {
            std::unique_lock<std::mutex> lock(round->mutex);
            round->changed.wait_until(lock, deadline, [&] {
                if (round->cancelled) return true;
                bool done = true;
                for (const Row& row : round->rows) {
                    if (!all && row.ok) return true;
                    done = done && row.done;
                }
                return done;
            });
            // --net only: transaction metrics arrive after the response callback, so give
            // finished rows up to 1 s to report proxy use. Normal probes never wait here.
            if (round->collect_metrics && !round->cancelled)
                round->changed.wait_for(lock, std::chrono::seconds(1), [&] {
                    if (round->cancelled) return true;
                    for (const Row& row : round->rows) if (row.done && row.proxy < 0) return false;
                    return true;
                });
            result.cancelled = round->cancelled;
            for (Row& row : round->rows) {
                if (!row.done) {
                    row.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - row.start).count();
                    row.timeout = !result.cancelled && Clock::now() >= deadline;
                    row.error = row.timeout ? L"探测截止超时" : L"本轮取消（已有成功地址或已停止）";
                }
                result.timeout = result.timeout || row.timeout;
                if (row.ok && (!result.ok || row.milliseconds < result.ms)) {
                    result.ok = true;
                    result.ms = row.milliseconds;
                }
            }
            result.rows = round->rows;
            round->cancelled = true; // Late callbacks cannot change the published result.
        }
        [session invalidateAndCancel]; // Never wait for NSURLSession callback queues on destruction.
        return result;
    }
}
struct PathState {
    std::mutex mutex;
    std::condition_variable changed;
    bool seen = false, wifi = false, wired = false, expensive = false;
    nw_path_status_t status = nw_path_status_invalid;
};
class PathMonitor {
public:
    std::shared_ptr<PathState> state = std::make_shared<PathState>();
    PathMonitor() {
        queue_ = dispatch_queue_create("Pulse.phase0.path", DISPATCH_QUEUE_SERIAL);
        monitor_ = nw_path_monitor_create();
        if (!monitor_ || !queue_) return;
        const auto shared = state;
        nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->seen = true;
            shared->status = nw_path_get_status(path);
            shared->wifi = nw_path_uses_interface_type(path, nw_interface_type_wifi);
            shared->wired = nw_path_uses_interface_type(path, nw_interface_type_wired);
            shared->expensive = nw_path_is_expensive(path);
            shared->changed.notify_all();
        });
        nw_path_monitor_set_queue(monitor_, queue_);
        nw_path_monitor_start(monitor_);
    }
    ~PathMonitor() { if (monitor_) nw_path_monitor_cancel(monitor_); }
    PathMonitor(const PathMonitor&) = delete;
    PathMonitor& operator=(const PathMonitor&) = delete;
private:
    nw_path_monitor_t monitor_ = nullptr;
    dispatch_queue_t queue_ = nullptr;
};
} // namespace probe_detail

void SetProbeRequireProxy(bool required) { probe_detail::require_proxy.store(required, std::memory_order_relaxed); }
bool ProbeRequireProxy() { return probe_detail::require_proxy.load(std::memory_order_relaxed); }

std::wstring BuildProbeDiagnosticsReport(const Settings& input) {
    using namespace probe_detail;
    const auto settings = Normalize(input);
    PathMonitor monitor;
    const Result result = Run(settings, std::make_shared<Round>(true), true);
    std::wostringstream out;
    out << L"require_proxy=" << result.require_proxy << L"\n";
    if (result.require_proxy) {
        out << L"本轮显式代理（-- 表示本轮未使用）：\n"
            << L"HTTP host=" << (result.http_proxy.host.empty() ? L"--" : result.http_proxy.host)
            << L" port=" << (result.http_proxy.port ? std::to_wstring(result.http_proxy.port) : L"--") << L"\n"
            << L"HTTPS host=" << (result.https_proxy.host.empty() ? L"--" : result.https_proxy.host)
            << L" port=" << (result.https_proxy.port ? std::to_wstring(result.https_proxy.port) : L"--") << L"\n"
            << L"proxy_rejected=" << result.proxy_rejected << L"\n";
    }
    NSDictionary* proxies = CFBridgingRelease(CFNetworkCopySystemProxySettings());
    const auto setting = [&](CFStringRef key) {
        id value = proxies[(__bridge NSString*)key];
        return value ? ToWide([value description]) : std::wstring(L"--");
    };
    out << L"系统代理设置（未配置的键为 --）：\n"
        << L"HTTP enabled=" << setting(kCFNetworkProxiesHTTPEnable)
        << L" host=" << setting(kCFNetworkProxiesHTTPProxy) << L" port=" << setting(kCFNetworkProxiesHTTPPort) << L"\n"
        << L"HTTPS enabled=" << setting(kCFNetworkProxiesHTTPSEnable)
        << L" host=" << setting(kCFNetworkProxiesHTTPSProxy) << L" port=" << setting(kCFNetworkProxiesHTTPSPort) << L"\n";
    id pac_enabled = proxies[(__bridge NSString*)kCFNetworkProxiesProxyAutoConfigEnable];
    id pac_url = proxies[(__bridge NSString*)kCFNetworkProxiesProxyAutoConfigURLString];
    out << L"PAC configured=" << (!proxies ? L"未知" : ([pac_enabled boolValue] || [pac_url length]) ? L"是" : L"否")
        << L" enabled=" << setting(kCFNetworkProxiesProxyAutoConfigEnable)
        << L" url=" << setting(kCFNetworkProxiesProxyAutoConfigURLString)
        << L" auto_discovery=" << setting(kCFNetworkProxiesProxyAutoDiscoveryEnable) << L"\n";
    if (!proxies) out << L"CFNetworkCopySystemProxySettings 失败（不能推断直连）\n";
    {
        std::unique_lock<std::mutex> lock(monitor.state->mutex);
        monitor.state->changed.wait_for(lock, std::chrono::milliseconds(500), [&] {return monitor.state->seen;});
        out << L"nw_path_monitor: seen=" << monitor.state->seen << L" status=";
        switch (monitor.state->status) {
        case nw_path_status_satisfied: out << L"satisfied"; break;
        case nw_path_status_unsatisfied: out << L"unsatisfied"; break;
        case nw_path_status_satisfiable: out << L"satisfiable"; break;
        default: out << L"invalid/unknown"; break;
        }
        out << L" wifi=" << monitor.state->wifi << L" wired=" << monitor.state->wired
            << L" expensive=" << monitor.state->expensive << L"（仅诊断，不影响 HTTP 判定）\n";
    }
    out << L"NSURLSession ephemeral / " << (result.require_proxy ? L"显式代理" : L"系统代理")
        << L" / 禁用缓存 / 仅 2xx / 拒绝重定向\n";
    // On M5 / macOS 26.6 proxyConnection stayed NO for plain HTTP through 127.0.0.1:7897,
    // while every transaction's remote endpoint was the proxy. Judge by the endpoint.
    const auto loopback = [](const std::wstring& host) {
        return host == L"127.0.0.1" || host == L"::1" || host == L"localhost" || host == L"::ffff:127.0.0.1";
    };
    const auto via_endpoint = [&](const Row& row) {
        if (!result.require_proxy || row.remote_address.empty() || row.remote_port.empty()) return -1;
        const ProxyEndpoint& proxy = row.url.compare(0, 6, L"https:") == 0 ? result.https_proxy : result.http_proxy;
        if (proxy.host.empty() || !proxy.port) return -1;
        const bool host = row.remote_address == proxy.host || (loopback(proxy.host) && loopback(row.remote_address));
        return host && row.remote_port == std::to_wstring(proxy.port) ? 1 : 0;
    };
    for (const Row& row : result.rows) {
        const int endpoint = via_endpoint(row);
        out << row.url << L" ms=" << row.milliseconds
        << L" HTTP=" << row.status << L" ok=" << row.ok
        << L" timeout=" << row.timeout << L" proxy_rejected=" << row.proxy_rejected
        << L" proxyConnection=" << (row.proxy < 0 ? L"未知" : row.proxy ? L"是" : L"否")
        << L" 经代理端点=" << (endpoint < 0 ? L"未知" : endpoint ? L"是" : L"否")
        << L" remote=" << (row.remote_address.empty() ? L"--" : row.remote_address)
        << L" port=" << (row.remote_port.empty() ? L"--" : row.remote_port) << L" error=" << row.error << L"\n";
        if (row.ok && (endpoint == 0 || (endpoint < 0 && row.proxy == 0))) out << L"警告：该请求未经代理\n";
    }
    out << L"经代理端点=是：该请求的远端地址和端口就是本轮显式代理（--require-proxy 时判断）。"
        << L"proxyConnection 是系统指标里的标志，普通 HTTP 经代理时可能为否，仅作参考。"
        << L"未知表示未发出请求或未收到路由指标（响应后最多等 1 秒），不推断为直连。\n";
    return out.str();
}
} // namespace mac

struct NetProbe::Impl {
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex;
    std::recursive_mutex publication; // Orders disable/sleep with user event publication.
    std::condition_variable changed;
    Settings settings;
    View view;
    bool stop = false, enabled = true, suspended = false;
    std::uint64_t epoch = 0;
    int failures = 0;
    Clock::time_point first_failure{};
    std::int64_t first_failure_unix = 0;
    Metrics::EventCallback callback = nullptr;
    void* user = nullptr;
    std::shared_ptr<mac::probe_detail::Round> active;
    std::thread worker;
    std::unique_ptr<mac::probe_detail::PathMonitor> monitor;
    void Reset() { view = {}; failures = 0; first_failure_unix = 0; ++epoch; }
    void Interrupt() { mac::probe_detail::Cancel(active); changed.notify_all(); }
    void Run() {
        using namespace mac::probe_detail;
        std::unique_lock<std::mutex> lock(mutex);
        auto due = Clock::now();
        while (!stop) {
            changed.wait(lock, [&] { return stop || (enabled && !suspended); });
            if (stop) break;
            const auto current_epoch = epoch;
            if (Clock::now() < due) {
                changed.wait_until(lock, due, [&] { return stop || epoch != current_epoch; });
                if (stop) break;
                if (epoch != current_epoch) { due = Clock::now(); continue; }
            }
            const Settings config = settings;
            const auto start = Clock::now();
            active = std::make_shared<Round>();
            const auto round = active;
            lock.unlock();
            const Result result = mac::probe_detail::Run(config, round, false);
            std::unique_lock<std::recursive_mutex> publish(publication);
            lock.lock();
            active.reset();
            if (stop || !enabled || suspended || current_epoch != epoch || result.cancelled) {
                due = Clock::now();
                lock.unlock(); publish.unlock(); lock.lock();
                continue;
            }
            NetEvent event{};
            bool notify = false;
            view.latency_ms = result.ok ? result.ms : NAN;
            const auto now = Clock::now();
            const auto unix_now = static_cast<std::int64_t>(std::time(nullptr));
            if (result.ok && result.ms < settings.unusable_ms) {
                if (view.offline_since_unix) {
                    notify = true; event.recovered = true;
                    event.reason = view.offline_reason;
                    event.latency_ms = view.offline_latency_ms;
                    event.offline_start_unix = view.offline_since_unix;
                    event.event_unix = unix_now;
                    event.outage_duration_s = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(now - first_failure).count());
                    event.message = L"网络已恢复 · " + LocalTime(unix_now) + L" · 本次断网 " + Duration(event.outage_duration_s);
                }
                failures = 0;
                view.offline_since_unix = 0;
                view.offline_reason = OfflineReason::None;
                view.offline_by_latency = false;
                view.offline_latency_ms = NAN;
                view.state = result.ms < settings.slow_ms ? NetState::Online : NetState::Slow;
            } else {
                if (!failures) { first_failure = now; first_failure_unix = unix_now; }
                failures = std::min(failures + 1, settings.fail_threshold);
                if (failures >= settings.fail_threshold) {
                    if (!view.offline_since_unix) {
                        view.offline_since_unix = first_failure_unix;
                        view.offline_reason = result.ok ? OfflineReason::HighLatency :
                            result.timeout ? OfflineReason::Timeout : OfflineReason::NoResponse;
                        view.offline_by_latency = result.ok;
                        view.offline_latency_ms = result.ok ? result.ms : NAN;
                        notify = true; event.recovered = false;
                        event.reason = view.offline_reason;
                        event.latency_ms = view.offline_latency_ms;
                        event.offline_start_unix = first_failure_unix;
                        event.event_unix = unix_now;
                        event.message = LocalTime(first_failure_unix) + L" 起无法访问网络";
                        if (result.ok) event.message += L"（延迟 " + std::to_wstring(static_cast<long long>(result.ms)) + L" ms）";
                        else if (result.timeout) event.message += L"（连接超时）";
                    }
                    view.state = NetState::Offline;
                } else if (result.ok) view.state = NetState::Slow;
            }
            due = start + std::chrono::seconds(settings.probe_interval_s);
            // A slow round never spins immediately; keep at least 1 ms before the next round.
            due = std::max(due, Clock::now() + std::chrono::milliseconds(1));
            const auto cb = callback;
            void* context = user;
            lock.unlock();
            if (notify) mac::probe_detail::AppendNetworkLog(
                std::wstring(event.recovered ? L"网络恢复：" : L"网络断开：") + event.message);
            if (notify && cb) cb(event, context);
            publish.unlock();
            lock.lock();
        }
    }
};

NetProbe::NetProbe() : impl_(new Impl) {}
NetProbe::~NetProbe() { Stop(); delete impl_; }
void NetProbe::Start(const Settings& settings, Metrics::EventCallback cb, void* user) {
    Stop();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->settings = mac::probe_detail::Normalize(settings);
    impl_->callback = cb; impl_->user = user; impl_->stop = false;
    impl_->Reset();
    impl_->monitor = std::make_unique<mac::probe_detail::PathMonitor>();
    mac::probe_detail::AppendNetworkLog(impl_->enabled ? L"启动监测：主动探测开启" : L"启动监测：主动探测关闭");
    impl_->worker = std::thread([p = impl_] { @autoreleasepool { p->Run(); } });
}
void NetProbe::Stop() {
    const bool was_running=impl_->worker.joinable();
    {
        std::lock_guard<std::recursive_mutex> publish(impl_->publication);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true; ++impl_->epoch; impl_->Interrupt();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->monitor.reset();
    if (was_running) mac::probe_detail::AppendNetworkLog(L"退出监测");
}
NetProbe::View NetProbe::Get() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->view;
}
std::wstring NetProbe::LogPath() const {
    @autoreleasepool { return mac::probe_detail::NetworkLogPath(); }
}
void NetProbe::SetSlowThreshold(int value) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->settings.slow_ms = std::clamp(value, 1, impl_->settings.unusable_ms - 1);
    if ((impl_->view.state == NetState::Online || impl_->view.state == NetState::Slow) &&
        std::isfinite(impl_->view.latency_ms) && impl_->view.latency_ms < impl_->settings.unusable_ms)
        impl_->view.state = impl_->view.latency_ms < impl_->settings.slow_ms ? NetState::Online : NetState::Slow;
}
void NetProbe::SetEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> publish(impl_->publication);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->enabled == enabled) return;
    impl_->enabled = enabled; impl_->Reset(); impl_->Interrupt();
}
void NetProbe::OnSuspend() {
    std::lock_guard<std::recursive_mutex> publish(impl_->publication);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->suspended = true; impl_->Reset(); impl_->Interrupt();
}
void NetProbe::OnResume() {
    std::lock_guard<std::recursive_mutex> publish(impl_->publication);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->suspended = false; impl_->Reset(); impl_->Interrupt();
}
} // namespace pulse
