#pragma once
#include "../src/metrics.h"
#include <string>

namespace pulse {
class NetProbe {
public:
    NetProbe();
    ~NetProbe();
    NetProbe(const NetProbe&) = delete;
    NetProbe& operator=(const NetProbe&) = delete;
    struct View {
        NetState state = NetState::Checking;
        double latency_ms = NAN;
        std::int64_t offline_since_unix = 0;
        bool offline_by_latency = false;
        OfflineReason offline_reason = OfflineReason::None;
        double offline_latency_ms = NAN;
    };
    void Start(const Settings&, Metrics::EventCallback, void*);
    void Stop();
    View Get() const;
    std::wstring LogPath() const;
    void SetSlowThreshold(int);
    void SetEnabled(bool);
    void OnSuspend();
    void OnResume();
private:
    struct Impl;
    Impl* impl_;
};
namespace mac {
// Test-only fail-closed proxy requirement; configure before starting Metrics.
void SetProbeRequireProxy(bool required);
bool ProbeRequireProxy();
std::wstring BuildProbeDiagnosticsReport(const Settings& settings);
}
}
