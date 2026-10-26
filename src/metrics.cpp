// Pulse metrics. PDH read failures publish unknown values, init retries every 30s,
// and queries are closed in the destructor.
// Optional metrics follow SetWants: disabled items stay unknown and drop their PDH/Wi-Fi handles.

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

#include "metrics.h"
#include "power_diagnostics.h"
#include "netprobe.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include "fpsmon.h"
#include <powrprof.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#include <netioapi.h>
#include <winioctl.h>
#include <setupapi.h>
#include <psapi.h>
#include <batclass.h>
#include <wlanapi.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#endif

#ifdef interface
#undef interface
#endif

#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xffff
#endif

namespace pulse {
namespace {

constexpr double kTauSec = 30.0;
constexpr double kMaxGapSec = 5.0;
constexpr double kAvgMinSec = 60.0;
constexpr unsigned long long kSchemeEveryMs = 5000;
constexpr unsigned long long kNicEveryMs = 30000;
constexpr unsigned long long kGpuEveryMs = 5000;
constexpr unsigned long long kEnergyEveryMs = 3000;
constexpr double kEnergyStaleSec = 5.0;
constexpr unsigned long long kPdhRetryMs = 30000;
constexpr unsigned long long kDiskEveryMs = 1000;
constexpr unsigned long long kSysDiskEveryMs = 30000;
constexpr unsigned long long kGpuMemEveryMs = 3000;
constexpr unsigned long long kGpuTempEveryMs = 2000;
constexpr unsigned long long kGpuTempRetryMs = 30000;
constexpr unsigned long long kGpuTempRediscoverMs = 60000;
constexpr unsigned long long kDgpuRetryMs = 30000;
constexpr unsigned long long kDgpuRediscoverMs = 60000;
constexpr unsigned long long kWifiEveryMs = 10000;
constexpr unsigned long long kBatteryHealthEveryMs = 600000;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr double kMiB = 1024.0 * 1024.0;
bool IsPhysicalNic(const MIB_IF_ROW2& row) {
    const auto& flags = row.InterfaceAndOperStatusFlags;
    return flags.HardwareInterface && !flags.FilterInterface &&
           row.Type != IF_TYPE_SOFTWARE_LOOPBACK && row.Type != IF_TYPE_TUNNEL;
}

constexpr unsigned kWantChip = 1u << 0;
constexpr unsigned kWantGpu = 1u << 1;
constexpr unsigned kWantProc = 1u << 2;
constexpr unsigned kWantMemDetail = 1u << 3;
constexpr unsigned kWantDisk = 1u << 4;
constexpr unsigned kWantSysDisk = 1u << 5;
constexpr unsigned kWantCpuFreq = 1u << 6;
constexpr unsigned kWantUptime = 1u << 7;
constexpr unsigned kWantGpuMem = 1u << 8;
constexpr unsigned kWantWifi = 1u << 9;
constexpr unsigned kWantBattery = 1u << 10;
constexpr unsigned kWantFps = 1u << 11;
constexpr unsigned kWantScheme = 1u << 12;
constexpr unsigned kWantCount = 1u << 13;
constexpr unsigned kWantGpuTemp = 1u << 14;
constexpr unsigned kWantDgpuPower = 1u << 15;
constexpr unsigned kWantDefault = kWantChip | kWantGpu | kWantProc;

static_assert(sizeof(void*) == 8, "x64 only");
static_assert(sizeof(BATTERY_INFORMATION) == 36, "BATTERY_INFORMATION size");
static_assert(offsetof(BATTERY_INFORMATION, DesignedCapacity) == 12, "DesignedCapacity offset");
static_assert(offsetof(BATTERY_INFORMATION, CycleCount) == 32, "CycleCount offset");
static_assert(sizeof(BATTERY_QUERY_INFORMATION) == 12, "BATTERY_QUERY_INFORMATION size");

// Windows SDK shared/d3dkmthk.h (Microsoft win32metadata, RecompiledIdlHeaders):
// https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/shared/d3dkmthk.h
// Local x64 ABI declarations: MinGW's SDK does not provide these newer types.
// KMTQUERYADAPTERINFOTYPE: ADAPTERTYPE = 15, ADAPTERPERFDATA = 62 (WDDM 2.4+).
constexpr UINT kPulseKmtAdapterType = 15;
constexpr UINT kPulseKmtAdapterPerfData = 62;
// ADAPTERTYPE's UINT bitfield: RenderSupported is bit 0; SoftwareDevice is
// the third bit (zero-based bit 2), not bit 3. Read masks to avoid bitfield ABI issues.
constexpr UINT kPulseKmtRenderSupported = 1u << 0;
constexpr UINT kPulseKmtSoftwareDevice = 1u << 2;
struct PulseKmtAdapterInfo {
    UINT hAdapter; // D3DKMT_HANDLE is a 32-bit UINT, not a Win32 HANDLE.
    LUID AdapterLuid;
    ULONG NumOfSources;
    BOOL bPrecisePresentRegionsPreferred;
};
struct PulseKmtEnumAdapters2 {
    ULONG NumAdapters;
    PulseKmtAdapterInfo* pAdapters;
};
struct PulseKmtQueryAdapterInfo {
    UINT hAdapter;
    UINT Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};
struct PulseKmtCloseAdapter { UINT hAdapter; };
struct PulseKmtAdapterType { UINT Value; };
struct PulseKmtAdapterPerfData {
    UINT32 PhysicalAdapterIndex;
    alignas(8) ULONGLONG MemoryFrequency;
    ULONGLONG MaxMemoryFrequency;
    ULONGLONG MaxMemoryFrequencyOC;
    ULONGLONG MemoryBandwidth;
    ULONGLONG PCIEBandwidth;
    ULONG FanRPM;      // Adapter fan only. This is not a general CPU or chassis fan.
    ULONG Power;       // 1 = 0.1% of the adapter's own scale. Never convert this to watts.
    ULONG Temperature; // 1 = 0.1 degrees Celsius; 0 means unavailable.
    UCHAR PowerStateOverride;
};
static_assert(sizeof(PulseKmtAdapterInfo) == 20, "D3DKMT_ADAPTERINFO size");
static_assert(offsetof(PulseKmtAdapterInfo, AdapterLuid) == 4, "AdapterLuid offset");
static_assert(offsetof(PulseKmtAdapterInfo, NumOfSources) == 12, "NumOfSources offset");
static_assert(offsetof(PulseKmtAdapterInfo, bPrecisePresentRegionsPreferred) == 16, "PrecisePresent offset");
static_assert(sizeof(PulseKmtEnumAdapters2) == 16, "D3DKMT_ENUMADAPTERS2 size");
static_assert(offsetof(PulseKmtEnumAdapters2, pAdapters) == 8, "pAdapters offset");
static_assert(sizeof(PulseKmtQueryAdapterInfo) == 24, "D3DKMT_QUERYADAPTERINFO size");
static_assert(offsetof(PulseKmtQueryAdapterInfo, Type) == 4, "Query Type offset");
static_assert(offsetof(PulseKmtQueryAdapterInfo, pPrivateDriverData) == 8, "PrivateDriverData offset");
static_assert(offsetof(PulseKmtQueryAdapterInfo, PrivateDriverDataSize) == 16, "PrivateDriverDataSize offset");
static_assert(sizeof(PulseKmtCloseAdapter) == 4, "D3DKMT_CLOSEADAPTER size");
static_assert(sizeof(PulseKmtAdapterType) == 4, "D3DKMT_ADAPTERTYPE size");
static_assert(sizeof(PulseKmtAdapterPerfData) == 64, "D3DKMT_ADAPTER_PERFDATA size");
static_assert(alignof(PulseKmtAdapterPerfData) == 8, "ADAPTER_PERFDATA alignment");
static_assert(offsetof(PulseKmtAdapterPerfData, MemoryFrequency) == 8, "MemoryFrequency offset");
static_assert(offsetof(PulseKmtAdapterPerfData, PCIEBandwidth) == 40, "PCIEBandwidth offset");
static_assert(offsetof(PulseKmtAdapterPerfData, FanRPM) == 48, "FanRPM offset");
static_assert(offsetof(PulseKmtAdapterPerfData, Power) == 52, "Power offset");
static_assert(offsetof(PulseKmtAdapterPerfData, Temperature) == 56, "Temperature offset");
static_assert(offsetof(PulseKmtAdapterPerfData, PowerStateOverride) == 60, "PowerStateOverride offset");

struct PulseKmtApi {
    // NTSTATUS is a signed 32-bit LONG; NT_SUCCESS(status) means status >= 0.
    using EnumFn = LONG(WINAPI*)(const PulseKmtEnumAdapters2*);
    using QueryFn = LONG(WINAPI*)(const PulseKmtQueryAdapterInfo*);
    using CloseFn = LONG(WINAPI*)(const PulseKmtCloseAdapter*);
    HMODULE module = nullptr;
    EnumFn enumerate = nullptr;
    QueryFn query = nullptr;
    CloseFn close = nullptr;
    PulseKmtApi() = default;
    PulseKmtApi(const PulseKmtApi&) = delete;
    PulseKmtApi& operator=(const PulseKmtApi&) = delete;
    ~PulseKmtApi() { Reset(); }
    bool Load() {
        if (module) return true;
        module = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
        enumerate = reinterpret_cast<EnumFn>(GetProcAddress(module, "D3DKMTEnumAdapters2"));
        query = reinterpret_cast<QueryFn>(GetProcAddress(module, "D3DKMTQueryAdapterInfo"));
        close = reinterpret_cast<CloseFn>(GetProcAddress(module, "D3DKMTCloseAdapter"));
        if (enumerate && query && close) return true;
        Reset();
        return false;
    }
    void Reset() {
        if (module) FreeLibrary(module);
        module = nullptr;
        enumerate = nullptr;
        query = nullptr;
        close = nullptr;
    }
};

void PulseKmtCloseAdapters(const PulseKmtApi& api, std::vector<PulseKmtAdapterInfo>& adapters) {
    for (auto& adapter : adapters) {
        if (adapter.hAdapter && api.close) {
            const PulseKmtCloseAdapter request{adapter.hAdapter};
            api.close(&request);
        }
        adapter.hAdapter = 0;
    }
    adapters.clear();
}

// CfgMgr ABI and property keys from the Windows SDK cfgmgr32.h/devpkey.h/winnt.h.
// https://github.com/microsoft/win32metadata/tree/main/generation/WinSDK/RecompiledIdlHeaders
constexpr DEVPROPKEY kPulseBusNumber = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 23};
constexpr DEVPROPKEY kPulseAddress = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 30};
constexpr DEVPROPKEY kPulsePowerData = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 32};
constexpr ULONG kPulseCmFilter = 0x00000200 | 0x00000100; // CLASS | PRESENT.
#if defined(CM_GETIDLIST_FILTER_CLASS) && defined(CM_GETIDLIST_FILTER_PRESENT)
static_assert(kPulseCmFilter == (CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT), "CM list flags");
#endif
static_assert(DN_STARTED == 0x00000008, "DN_STARTED value");
static_assert(PowerDeviceD0 == kDgpuPowerD0, "PowerDeviceD0 value");
static_assert(sizeof(DEVICE_POWER_STATE) == 4 && sizeof(SYSTEM_POWER_STATE) == 4, "Power enum sizes");
// All fields are 32 bits; no pointer-sized fields in CM_POWER_DATA.
struct PulseCmPowerData {
    DWORD PD_Size, PD_MostRecentPowerState, PD_Capabilities;
    DWORD PD_D1Latency, PD_D2Latency, PD_D3Latency;
    DWORD PD_PowerStateMapping[7], PD_DeepestSystemWake;
};
static_assert(sizeof(DEVPROPKEY) == 20 && offsetof(DEVPROPKEY, pid) == 16, "DEVPROPKEY layout");
static_assert(sizeof(PulseCmPowerData) == 56, "CM_POWER_DATA size");
static_assert(offsetof(PulseCmPowerData, PD_MostRecentPowerState) == 4, "CM power state offset");
static_assert(offsetof(PulseCmPowerData, PD_PowerStateMapping) == 24, "CM power mapping offset");
static_assert(offsetof(PulseCmPowerData, PD_DeepestSystemWake) == 52, "CM wake state offset");

struct PulseCmApi {
    HMODULE module = nullptr;
    decltype(&CM_Get_Device_ID_List_SizeW) size = nullptr;
    decltype(&CM_Get_Device_ID_ListW) list = nullptr;
    decltype(&CM_Locate_DevNodeW) locate = nullptr;
    decltype(&CM_Get_DevNode_Status) status = nullptr;
    decltype(&CM_Get_DevNode_PropertyW) property = nullptr;
    DWORD load_error = 0;
    PulseCmApi() = default;
    PulseCmApi(const PulseCmApi&) = delete;
    PulseCmApi& operator=(const PulseCmApi&) = delete;
    ~PulseCmApi() { Reset(); }
    void Reset() {
        if (module) FreeLibrary(module);
        module = nullptr;
        size = nullptr; list = nullptr; locate = nullptr; status = nullptr; property = nullptr;
    }
    bool Load() {
        if (module) return true;
        load_error = 0;
        module = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { load_error = GetLastError(); return false; }
        size = reinterpret_cast<decltype(size)>(GetProcAddress(module, "CM_Get_Device_ID_List_SizeW"));
        list = reinterpret_cast<decltype(list)>(GetProcAddress(module, "CM_Get_Device_ID_ListW"));
        locate = reinterpret_cast<decltype(locate)>(GetProcAddress(module, "CM_Locate_DevNodeW"));
        status = reinterpret_cast<decltype(status)>(GetProcAddress(module, "CM_Get_DevNode_Status"));
        property = reinterpret_cast<decltype(property)>(GetProcAddress(module, "CM_Get_DevNode_PropertyW"));
        if (size && list && locate && status && property) return true;
        load_error = ERROR_PROC_NOT_FOUND;
        Reset();
        return false;
    }
};
struct PulseCmNode {
    std::wstring id;
    DEVINST devinst = 0;
    ULONG flags = 0, problem = 0, bus = 0, address = 0;
    CONFIGRET locate_rc = CR_CALL_NOT_IMPLEMENTED, status_rc = CR_CALL_NOT_IMPLEMENTED;
    CONFIGRET bus_rc = CR_CALL_NOT_IMPLEMENTED, address_rc = CR_CALL_NOT_IMPLEMENTED;
    CONFIGRET power_rc = CR_CALL_NOT_IMPLEMENTED;
    bool eligible = false;
};
struct DgpuGate {
    PulseCmApi api;
    std::vector<PulseCmNode> nodes;
    std::vector<DgpuAdapter> adapters;
    bool ready = false;
    unsigned long long retry_at = 0, enum_at = 0;
    CONFIGRET enum_rc = CR_CALL_NOT_IMPLEMENTED;
    void Reset() {
        api.Reset();
        std::vector<PulseCmNode>().swap(nodes);
        std::vector<DgpuAdapter>().swap(adapters);
        ready = false;
        retry_at = enum_at = 0;
        enum_rc = CR_CALL_NOT_IMPLEMENTED;
    }
    bool Fail(unsigned long long now) {
        ready = false;
        retry_at = now + kDgpuRetryMs;
        return false;
    }
    CONFIGRET ReadUint(DEVINST devinst, const DEVPROPKEY& key, ULONG& value) {
        DEVPROPTYPE type = 0;
        ULONG bytes = sizeof(value);
        const CONFIGRET rc = api.property(devinst, &key, &type, reinterpret_cast<PBYTE>(&value), &bytes, 0);
        if (rc != CR_SUCCESS) return rc;
        return type == DEVPROP_TYPE_UINT32 && bytes == sizeof(value) ? CR_SUCCESS : CR_INVALID_DATA;
    }
    bool Ensure(unsigned long long now) {
        if (ready) return true;
        if (now < retry_at) return false;
        nodes.clear(); adapters.clear();
        if (!api.Load()) { enum_rc = CR_CALL_NOT_IMPLEMENTED; return Fail(now); }
        const wchar_t* display_class = L"{4d36e968-e325-11ce-bfc1-08002be10318}";
        std::vector<wchar_t> ids;
        // Device arrival can grow the MULTI_SZ between the two calls.
        for (int attempt = 0; attempt < 3; ++attempt) {
            ULONG count = 0;
            enum_rc = api.size(&count, display_class, kPulseCmFilter);
            if (enum_rc != CR_SUCCESS) return Fail(now);
            if (!count || count > (1u << 20)) { enum_rc = CR_INVALID_DATA; return Fail(now); }
            ids.assign(count, L'\0');
            enum_rc = api.list(display_class, ids.data(), count, kPulseCmFilter);
            if (enum_rc == CR_SUCCESS) break;
            if (enum_rc != CR_BUFFER_SMALL) return Fail(now);
        }
        if (enum_rc != CR_SUCCESS) return Fail(now);
        bool failed = false;
        for (std::size_t pos = 0; pos < ids.size() && ids[pos];) {
            const wchar_t* end = std::wmemchr(ids.data() + pos, L'\0', ids.size() - pos);
            if (!end) { enum_rc = CR_INVALID_DATA; return Fail(now); }
            PulseCmNode node;
            node.id.assign(ids.data() + pos, static_cast<std::size_t>(end - (ids.data() + pos)));
            pos += node.id.size() + 1;
            if (node.id.size() < 12 || _wcsnicmp(node.id.c_str(), L"PCI\\VEN_10DE", 12) != 0) continue;
            node.locate_rc = api.locate(&node.devinst, node.id.data(), 0);
            if (node.locate_rc == CR_SUCCESS)
                node.status_rc = api.status(&node.flags, &node.problem, node.devinst, 0);
            if (node.status_rc == CR_SUCCESS && (node.flags & DN_STARTED)) {
                node.bus_rc = ReadUint(node.devinst, kPulseBusNumber, node.bus);
                node.address_rc = ReadUint(node.devinst, kPulseAddress, node.address);
                node.eligible = node.bus_rc == CR_SUCCESS && node.address_rc == CR_SUCCESS;
                failed = failed || !node.eligible;
            }
            failed = failed || node.locate_rc != CR_SUCCESS || node.status_rc != CR_SUCCESS;
            adapters.push_back({0x10DE, node.bus, node.address >> 16, 0, false});
            nodes.push_back(std::move(node));
        }
        if (failed) return Fail(now);
        ready = true;
        enum_at = now;
        retry_at = 0;
        return true;
    }
    DgpuStatus Read(unsigned long long now) {
        diagnostics::ScopedTiming timing(L"CfgMgr dGPU gate");
        // Empty/disabled lists need discovery after a MUX or PnP change.
        bool have_started = false;
        for (const auto& node : nodes) have_started = have_started || node.eligible;
        if (ready && !have_started && now - enum_at >= kDgpuRediscoverMs) ready = false;
        if (!Ensure(now)) return DgpuStatus::ReadFailed;
        bool failed = false;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            auto& adapter = adapters[i];
            adapter.power_state = 0;
            adapter.power_read_ok = false;
            if (!node.eligible) continue;
            PulseCmPowerData data{};
            data.PD_Size = sizeof(data);
            DEVPROPTYPE type = 0;
            ULONG bytes = sizeof(data);
            node.power_rc = api.property(node.devinst, &kPulsePowerData, &type,
                reinterpret_cast<PBYTE>(&data), &bytes, 0);
            if (node.power_rc == CR_SUCCESS && (type != DEVPROP_TYPE_BINARY || bytes != sizeof(data) ||
                data.PD_Size != sizeof(data))) node.power_rc = CR_INVALID_DATA;
            if (node.power_rc != CR_SUCCESS) { failed = true; continue; }
            adapter.power_state = data.PD_MostRecentPowerState;
            adapter.power_read_ok = true;
        }
        if (failed) { Fail(now); return DgpuStatus::ReadFailed; }
        return DgpuGateStatus(adapters);
    }
};

const wchar_t* DgpuPowerStateName(const DgpuAdapter& adapter) {
    if (!adapter.power_read_ok) return L"unknown";
    switch (adapter.power_state) {
    case 1: return L"D0";
    case 2: return L"D1";
    case 3: return L"D2";
    case 4: return L"D3";
    default: return L"Unspecified";
    }
}
std::wstring DgpuCrText(CONFIGRET rc) {
    const wchar_t* name = L"CR_OTHER";
    switch (rc) {
    case CR_SUCCESS: name = L"CR_SUCCESS"; break;
    case CR_NO_SUCH_DEVINST: name = L"CR_NO_SUCH_DEVINST"; break;
    case CR_BUFFER_SMALL: name = L"CR_BUFFER_SMALL"; break;
    case CR_INVALID_DATA: name = L"CR_INVALID_DATA"; break;
    case CR_INVALID_DEVNODE: name = L"CR_INVALID_DEVNODE"; break;
    case CR_NO_SUCH_VALUE: name = L"CR_NO_SUCH_VALUE"; break;
    case CR_CALL_NOT_IMPLEMENTED: name = L"CR_CALL_NOT_IMPLEMENTED"; break;
    case CR_DEVICE_NOT_THERE: name = L"CR_DEVICE_NOT_THERE"; break;
    case CR_ACCESS_DENIED: name = L"CR_ACCESS_DENIED"; break;
    }
    wchar_t text[96]{};
    std::swprintf(text, 96, L"%ls (0x%08lX)", name, static_cast<unsigned long>(rc));
    return text;
}
std::wstring DgpuGateReport(const DgpuGate& gate, DgpuStatus status) {
    std::wstring report = std::wstring(L"CfgMgr D-state gate: ") + DgpuStatusName(status) + L"\r\n";
    report += L"  enumeration=" + DgpuCrText(gate.enum_rc) + L" load_error=" +
        std::to_wstring(gate.api.load_error) + L"\r\n";
    report += L"  CR_CALL_NOT_IMPLEMENTED also marks skipped calls; CR_INVALID_DATA includes invalid property type/size.\r\n";
    for (std::size_t i = 0; i < gate.nodes.size(); ++i) {
        const auto& node = gate.nodes[i];
        const auto& adapter = gate.adapters[i];
        report += L"  Instance ID: " + node.id + L"\r\n";
        report += L"    locate=" + DgpuCrText(node.locate_rc) + L" status=" + DgpuCrText(node.status_rc) + L"\r\n";
        report += std::wstring(L"    DN_STARTED=") + ((node.flags & DN_STARTED) ? L"yes" : L"no") +
            L" flags=" + std::to_wstring(node.flags) + L" problem=" + std::to_wstring(node.problem) + L"\r\n";
        report += L"    Bus=" + std::to_wstring(node.bus) + L" Device=" + std::to_wstring(node.address >> 16) +
            L" Function=" + std::to_wstring(node.address & 0xffff) + L" Address=" + std::to_wstring(node.address) + L"\r\n";
        report += L"    Bus_rc=" + DgpuCrText(node.bus_rc) + L" Address_rc=" + DgpuCrText(node.address_rc) + L"\r\n";
        report += std::wstring(L"    PowerState=") + DgpuPowerStateName(adapter) + L" raw=" +
            std::to_wstring(adapter.power_state) + L" PowerData_rc=" + DgpuCrText(node.power_rc) + L"\r\n";
    }
    return report;
}

// NVML ABI only; no NVIDIA headers, import library, or default DLL search path.
// https://github.com/NVIDIA/go-nvml/blob/main/pkg/nvml/nvml.h (nvmlPciInfo_st)
using nvmlReturn_t = int;
struct nvmlDevice_st;
using nvmlDevice_t = nvmlDevice_st*;
struct nvmlPciInfo_t {
    char busIdLegacy[16];
    unsigned domain, bus, device, pciDeviceId, pciSubSystemId;
    char busId[32];
};
static_assert(sizeof(nvmlPciInfo_t) == 68, "nvmlPciInfo_t size");
static_assert(offsetof(nvmlPciInfo_t, domain) == 16, "NVML domain offset");
static_assert(offsetof(nvmlPciInfo_t, bus) == 20, "NVML bus offset");
static_assert(offsetof(nvmlPciInfo_t, device) == 24, "NVML device offset");
static_assert(offsetof(nvmlPciInfo_t, pciDeviceId) == 28, "NVML pciDeviceId offset");
static_assert(offsetof(nvmlPciInfo_t, pciSubSystemId) == 32, "NVML pciSubSystemId offset");
static_assert(offsetof(nvmlPciInfo_t, busId) == 36, "NVML busId offset");
constexpr nvmlReturn_t kNvmlSuccess = kDgpuNvmlSuccess;

struct NvmlApi {
    using InitFn = nvmlReturn_t(*)();
    using ShutdownFn = nvmlReturn_t(*)();
    using CountFn = nvmlReturn_t(*)(unsigned*);
    using HandleFn = nvmlReturn_t(*)(unsigned, nvmlDevice_t*);
    using PciFn = nvmlReturn_t(*)(nvmlDevice_t, nvmlPciInfo_t*);
    using PowerFn = nvmlReturn_t(*)(nvmlDevice_t, unsigned*);
    using NameFn = nvmlReturn_t(*)(nvmlDevice_t, char*, unsigned);
    HMODULE module = nullptr;
    InitFn init = nullptr;
    ShutdownFn shutdown = nullptr;
    CountFn count = nullptr;
    HandleFn handle = nullptr;
    PciFn pci = nullptr;
    PowerFn power = nullptr;
    PowerFn enforced_limit = nullptr;
    NameFn name = nullptr;
    bool initialized = false;
    nvmlReturn_t init_result = -1;
    DWORD load_error = 0;
    std::wstring path;
    NvmlApi() = default;
    NvmlApi(const NvmlApi&) = delete;
    NvmlApi& operator=(const NvmlApi&) = delete;
    ~NvmlApi() { Reset(); }
    void Reset() {
        if (initialized && shutdown) shutdown();
        initialized = false;
        if (module) FreeLibrary(module);
        module = nullptr;
        init = nullptr; shutdown = nullptr; count = nullptr; handle = nullptr;
        pci = nullptr; power = nullptr; enforced_limit = nullptr; name = nullptr;
    }
    bool Open() {
        diagnostics::ScopedTiming timing(L"NVML init");
        Reset();
        path = L"System32\\nvml.dll";
        init_result = -1;
        load_error = 0;
        module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            wchar_t root[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"ProgramW6432", root, 32768);
            // Accept only an absolute drive path, never an expanded relative path.
            if (length >= 3 && length < 32768 && root[1] == L':' && root[2] == L'\\') {
                path = std::wstring(root) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
                module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }
        if (!module) { load_error = GetLastError(); return false; }
        wchar_t actual[32768]{};
        const DWORD length = GetModuleFileNameW(module, actual, 32768);
        if (length > 0 && length < 32768) path.assign(actual, length);
        init = reinterpret_cast<InitFn>(GetProcAddress(module, "nvmlInit_v2"));
        shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(module, "nvmlShutdown"));
        count = reinterpret_cast<CountFn>(GetProcAddress(module, "nvmlDeviceGetCount_v2"));
        handle = reinterpret_cast<HandleFn>(GetProcAddress(module, "nvmlDeviceGetHandleByIndex_v2"));
        pci = reinterpret_cast<PciFn>(GetProcAddress(module, "nvmlDeviceGetPciInfo_v3"));
        power = reinterpret_cast<PowerFn>(GetProcAddress(module, "nvmlDeviceGetPowerUsage"));
        enforced_limit = reinterpret_cast<PowerFn>(GetProcAddress(module, "nvmlDeviceGetEnforcedPowerLimit"));
        name = reinterpret_cast<NameFn>(GetProcAddress(module, "nvmlDeviceGetName"));
        if (!init || !shutdown || !count || !handle || !pci || !power || !name) {
            load_error = ERROR_PROC_NOT_FOUND;
            Reset();
            return false;
        }
        init_result = init();
        initialized = init_result == kNvmlSuccess;
        if (!initialized) { Reset(); return false; }
        return true;
    }
};

struct NvmlBoard {
    nvmlDevice_t handle = nullptr;
    nvmlPciInfo_t pci{};
    unsigned limit_mw = 0;
    bool limit_known = false;
    bool selected = false;
};

struct DgpuPower {
    DgpuGate gate;
    NvmlApi nvml;
    std::vector<NvmlBoard> boards;
    std::vector<DgpuDevice> readings;
    unsigned long long retry_at = 0;
    DgpuStatus last_gate = DgpuStatus::Off;
    DgpuHistory history;
    bool glitch_logged = false;
    unsigned long long glitch_log_at = 0;
    DgpuStatus logged_status = DgpuStatus::Off;
    std::size_t logged_matched = static_cast<std::size_t>(-1);
    std::wstring logged_init;
    std::wstring logged_devices;
    DgpuStatus retry_status = DgpuStatus::NvmlUnavailable;
    void CloseNvml() {
        nvml.Reset();
        boards.clear();
        readings.clear();
        history.Reset();
    }
    void Reset() {
        CloseNvml();
        std::vector<NvmlBoard>().swap(boards);
        std::vector<DgpuDevice>().swap(readings);
        gate.Reset();
        retry_at = 0;
        last_gate = DgpuStatus::Off;
        glitch_logged = false;
        glitch_log_at = 0;
        logged_status = DgpuStatus::Off;
        logged_matched = static_cast<std::size_t>(-1);
        logged_init.clear();
        logged_devices.clear();
    }
    DgpuResult Publish(DgpuResult result) {
        // Sleep transitions have their own gate log; glitches use the rate-limited raw log.
        if (result.status != DgpuStatus::Sleeping && result.status != DgpuStatus::Valid &&
            result.status != DgpuStatus::Glitch &&
            (result.status != logged_status || result.matched != logged_matched)) {
            diagnostics::WriteLog(L"GPU", std::wstring(L"dGPU status=") + DgpuStatusName(result.status) +
                L" matched=" + std::to_wstring(result.matched));
        }
        logged_status = result.status;
        logged_matched = result.matched;
        return result;
    }
    bool Enumerate() {
        boards.clear();
        unsigned count = 0;
        const nvmlReturn_t count_rc = nvml.count(&count);
        bool ok = count_rc == kNvmlSuccess && count <= 4096;
        std::wstring message = L"NVML devices=" + std::to_wstring(count) + L" count_rc=" + std::to_wstring(count_rc);
        if (ok) {
            for (unsigned i = 0; i < count; ++i) {
                NvmlBoard board;
                const nvmlReturn_t handle_rc = nvml.handle(i, &board.handle);
                const nvmlReturn_t pci_rc = handle_rc == kNvmlSuccess && board.handle ?
                    nvml.pci(board.handle, &board.pci) : -1;
                message += L" [" + std::to_wstring(i) + L" handle_rc=" + std::to_wstring(handle_rc) +
                    L" pci_rc=" + std::to_wstring(pci_rc) + L" bus=" + std::to_wstring(board.pci.bus) +
                    L" device=" + std::to_wstring(board.pci.device) + L"]";
                if (handle_rc != kNvmlSuccess || pci_rc != kNvmlSuccess) { ok = false; continue; }
                // CfgMgr exposes bus/device without a PCI domain; require domain 0.
                if (board.pci.domain == 0) {
                    if (DgpuMatches(gate.adapters, board.pci.bus, board.pci.device) && nvml.enforced_limit) {
                        unsigned limit = 0;
                        if (nvml.enforced_limit(board.handle, &limit) == kNvmlSuccess) {
                            board.limit_mw = limit;
                            board.limit_known = true;
                        }
                    }
                    boards.push_back(board);
                }
            }
        }
        if (message != logged_devices) { diagnostics::WriteLog(L"GPU", message); logged_devices = message; }
        return ok;
    }
    DgpuResult Read(unsigned long long now) {
        const DgpuStatus status = gate.Read(now);
        if (status == DgpuStatus::Sleeping && last_gate != DgpuStatus::Sleeping)
            diagnostics::WriteLog(L"GPU", L"dGPU asleep: NVML idle, no calls");
        else if (status == DgpuStatus::Valid && last_gate != DgpuStatus::Valid)
            diagnostics::WriteLog(L"GPU", L"dGPU awake");
        last_gate = status;
        const auto action = DgpuNextAction(status == DgpuStatus::Valid, nvml.initialized, now, retry_at);
        if (status != DgpuStatus::Valid) {
            // Sleeping keeps the session untouched, including nvmlShutdown.
            // Missing/failed PnP devices invalidate the session on this exceptional path.
            if (status != DgpuStatus::Sleeping) CloseNvml();
            return Publish({status, NAN, 0});
        }
        if (action == DgpuAction::None) return Publish({retry_status, NAN, 0});
        if (action == DgpuAction::Init) {
            retry_at = now + kDgpuRetryMs;
            const bool opened = nvml.Open();
            const std::wstring message = L"NVML path=" + nvml.path + L" init_rc=" +
                std::to_wstring(nvml.init_result) + L" load_error=" + std::to_wstring(nvml.load_error);
            if (message != logged_init) { diagnostics::WriteLog(L"GPU", message); logged_init = message; }
            if (!opened) {
                retry_status = DgpuStatus::NvmlUnavailable;
                return Publish({retry_status, NAN, 0});
            }
            if (!Enumerate()) {
                CloseNvml();
                retry_status = DgpuStatus::ReadFailed;
                return Publish({retry_status, NAN, 0});
            }
        }
        readings.clear();
        {
            diagnostics::ScopedTiming timing(L"NVML power");
            for (auto& board : boards) {
                const bool selected = DgpuMatches(gate.adapters, board.pci.bus, board.pci.device);
                if (selected != board.selected) history.Reset();
                board.selected = selected;
                if (!selected) continue;
                unsigned mw = 0;
                const nvmlReturn_t rc = nvml.power(board.handle, &mw);
                readings.push_back({board.pci.bus, board.pci.device, rc, mw, board.limit_mw, board.limit_known});
                if (rc == kNvmlSuccess && DgpuPowerGlitch(mw, board.limit_mw, board.limit_known) &&
                    (!glitch_logged || now - glitch_log_at >= 60000)) {
                    diagnostics::WriteLog(L"GPU", L"dGPU power glitch: raw_mW=" + std::to_wstring(mw) +
                        L" limit_mW=" + std::to_wstring(board.limit_mw) +
                        L" limit_known=" + (board.limit_known ? L"yes" : L"no"));
                    glitch_logged = true;
                    glitch_log_at = now;
                }
            }
        }
        const DgpuResult result = history.Apply(InterpretDgpu(gate.adapters, readings), now);
        if (result.status == DgpuStatus::ReadFailed || result.status == DgpuStatus::NoMatch) {
            // A driver reset/hotplug may invalidate cached handles or PCI identities.
            CloseNvml();
            retry_at = now + kDgpuRetryMs;
            retry_status = result.status;
        }
        return Publish(result);
    }
};

struct BatteryReading {
    bool present = false;
    bool ac_known = false;
    bool ac = false;
    bool charging = false;
    bool discharging = false;
    bool rate_known = false;
    double rate_mw = 0;
    double remaining_mwh = 0;
    double full_mwh = 0;
    int percent = -1;
    double life_s = -1;
    bool saver = false;
};

struct RawBatteryState {
    unsigned char AcOnLine;
    unsigned char BatteryPresent;
    unsigned char Charging;
    unsigned char Discharging;
    unsigned char Spare1[3];
    unsigned char Tag;
    unsigned long MaxCapacity;
    unsigned long RemainingCapacity;
    long Rate;
    unsigned long EstimatedTime;
    unsigned long DefaultAlert1;
    unsigned long DefaultAlert2;
};

static_assert(sizeof(RawBatteryState) == 32, "SYSTEM_BATTERY_STATE size");
static_assert(offsetof(RawBatteryState, MaxCapacity) == 8, "MaxCapacity offset");
static_assert(offsetof(RawBatteryState, Rate) == 16, "Rate offset");

bool ReadBattery(BatteryReading& out) {
    RawBatteryState bs{};
    SYSTEM_POWER_STATUS ps{};
    const DWORD rc = CallNtPowerInformation(static_cast<POWER_INFORMATION_LEVEL>(5), nullptr, 0, &bs,
                                            static_cast<ULONG>(sizeof(bs)));
    if (rc != 0) return false;
    if (!GetSystemPowerStatus(&ps)) return false;
    out.present = bs.BatteryPresent != 0;
    // ACLineStatus 255 is unknown. If it disagrees with SYSTEM_BATTERY_STATE,
    // do not pick a side: the sample must not claim a confirmed AC line.
    const bool nt_ac = bs.AcOnLine != 0;
    if (ps.ACLineStatus == 0 || ps.ACLineStatus == 1) {
        const bool status_ac = ps.ACLineStatus == 1;
        out.ac_known = status_ac == nt_ac;
        out.ac = out.ac_known && status_ac;
    }
    out.charging = bs.Charging != 0;
    out.discharging = bs.Discharging != 0;
    // Unknown sentinels stay unknown. Do not publish them as 0 W.
    out.rate_known = BatteryRateKnown(bs.Rate);
    out.rate_mw = out.rate_known ? static_cast<double>(bs.Rate) : 0.0;
    out.remaining_mwh = static_cast<double>(bs.RemainingCapacity);
    out.full_mwh = static_cast<double>(bs.MaxCapacity);
    out.percent = ps.BatteryLifePercent <= 100 ? static_cast<int>(ps.BatteryLifePercent) : -1;
    out.life_s = ps.BatteryLifeTime != 0xFFFFFFFFu ? static_cast<double>(ps.BatteryLifeTime) : -1.0;
    out.saver = ps.SystemStatusFlag == 1;
    return true;
}

using OverlayFn = DWORD(WINAPI*)(GUID*);
using ActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
using FriendlyNameFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);

// Byte-capped Unicode read. A failed or empty name must not keep a previous plan.
bool ReadPlanName(FriendlyNameFn fn, const GUID& guid, std::wstring& out) {
    out.clear();
    if (!fn) return false;
    DWORD bytes = 0;
    const DWORD first = fn(nullptr, &guid, nullptr, nullptr, nullptr, &bytes);
    if ((first != ERROR_SUCCESS && first != ERROR_MORE_DATA) ||
        bytes < sizeof(wchar_t) || bytes > 2048 || (bytes % sizeof(wchar_t)) != 0) return false;
    std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes) + sizeof(wchar_t), 0);
    DWORD got = bytes;
    if (fn(nullptr, &guid, nullptr, nullptr, buffer.data(), &got) != ERROR_SUCCESS) return false;
    if (got > bytes) got = bytes;
    const auto* text = reinterpret_cast<const wchar_t*>(buffer.data());
    const std::size_t chars = got / sizeof(wchar_t);
    std::size_t length = 0;
    while (length < chars && text[length] != L'\0') ++length;
    if (length == 0) return false;
    out.assign(text, length);
    return true;
}

bool SameGuid(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

GUID MakeGuid(unsigned long d1, unsigned short d2, unsigned short d3, unsigned char b0,
              unsigned char b1, unsigned char b2, unsigned char b3, unsigned char b4,
              unsigned char b5, unsigned char b6, unsigned char b7) {
    GUID g{};
    g.Data1 = d1;
    g.Data2 = d2;
    g.Data3 = d3;
    g.Data4[0] = b0;
    g.Data4[1] = b1;
    g.Data4[2] = b2;
    g.Data4[3] = b3;
    g.Data4[4] = b4;
    g.Data4[5] = b5;
    g.Data4[6] = b6;
    g.Data4[7] = b7;
    return g;
}

PowerSchemeKind SchemeKind(const GUID& guid) {
    const GUID best_eff =
        MakeGuid(0x961cc777, 0x2547, 0x4f9d, 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a);
    const GUID longer =
        MakeGuid(0x3af9b8d9, 0x7c97, 0x431d, 0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f);
    const GUID balanced = MakeGuid(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const GUID best_perf =
        MakeGuid(0xded574b5, 0x45a0, 0x4f42, 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38);
    if (SameGuid(guid, best_eff)) return PowerSchemeKind::BestEfficiency;
    if (SameGuid(guid, longer)) return PowerSchemeKind::LongerBattery;
    if (SameGuid(guid, balanced)) return PowerSchemeKind::Balanced;
    if (SameGuid(guid, best_perf)) return PowerSchemeKind::BestPerformance;
    return PowerSchemeKind::Unknown;
}

// Keep the legacy Chinese value for diagnostics/self-test consumers.
std::wstring SchemeName(PowerSchemeKind kind) {
    switch (kind) {
    case PowerSchemeKind::BestEfficiency: return L"\u6700\u4F73\u80FD\u6548";
    case PowerSchemeKind::LongerBattery: return L"\u66F4\u957F\u7EED\u822A";
    case PowerSchemeKind::Balanced: return L"\u5E73\u8861";
    case PowerSchemeKind::BestPerformance: return L"\u6700\u4F73\u6027\u80FD";
    default: return L"";
    }
}

wchar_t LowerChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

bool IsWordEnd(wchar_t c) {
    if (c >= L'0' && c <= L'9') return false;
    if (c >= L'A' && c <= L'Z') return false;
    if (c >= L'a' && c <= L'z') return false;
    return true;
}

bool ContainsToken(const wchar_t* text, const wchar_t* token) {
    if (!text || !token || !token[0]) return false;
    for (std::size_t i = 0; text[i]; ++i) {
        if (i > 0 && !IsWordEnd(text[i - 1])) continue;
        std::size_t j = 0;
        for (; token[j]; ++j) {
            const wchar_t c = text[i + j];
            if (!c || LowerChar(c) != LowerChar(token[j])) break;
        }
        if (!token[j] && IsWordEnd(text[i + j])) return true;
    }
    return false;
}

int MatchScore(const wchar_t* name, const wchar_t* token) {
    if (!ContainsToken(name, token)) return 0;
    return ContainsToken(name, L"package0") ? 2 : 1;
}

bool FormatArray(PDH_HCOUNTER counter, std::vector<unsigned char>& buffer, DWORD& count,
                 DWORD format = PDH_FMT_DOUBLE, long* error = nullptr) {
    if (error) *error = PDH_INVALID_HANDLE;
    if (!counter) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD need = static_cast<DWORD>(buffer.size());
        count = 0;
        const PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, format, &need, &count,
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
        if (error) *error = status;
        if (status == ERROR_SUCCESS) return true;
        if (static_cast<DWORD>(status) != static_cast<DWORD>(PDH_MORE_DATA) ||
            need == 0 || need > (16u << 20)) return false;
        // Keep the allocation for later collections; instances can appear mid-read.
        buffer.resize(std::max(static_cast<std::size_t>(need), buffer.size() * 2));
    }
    return false;
}

bool QueryArray(PDH_HQUERY query, PDH_HCOUNTER counter, std::vector<unsigned char>& buffer, DWORD& count,
                DWORD format = PDH_FMT_DOUBLE) {
    if (!query || !counter) return false;
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) return false;
    return FormatArray(counter, buffer, count, format);
}

bool FormatDouble(PDH_HCOUNTER counter, double& out, DWORD format = PDH_FMT_DOUBLE) {
    if (!counter) return false;
    PDH_FMT_COUNTERVALUE value{};
    const PDH_STATUS status = PdhGetFormattedCounterValue(counter, format, nullptr, &value);
    if (status != ERROR_SUCCESS) return false;
    if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) return false;
    if (!std::isfinite(value.doubleValue)) return false;
    out = value.doubleValue;
    return true;
}

// Caller guarantees !have || now >= due, so the subtraction does not wrap.
unsigned long long NextDue(bool have, unsigned long long due, unsigned long long now, unsigned long long every) {
    if (have && now - due < every) return due + every;
    return now + every;
}

enum class PdhRead { Ok, Warmup, Fail };

struct CounterBind {
    const wchar_t* path;
    PDH_HCOUNTER* slot;
};

bool OpenEnglishQuery(PDH_HQUERY& query, const CounterBind* binds, int count) {
    if (query) {
        PdhCloseQuery(query);
        query = nullptr;
    }
    for (int i = 0; i < count; ++i) {
        if (binds[i].slot) *binds[i].slot = nullptr;
    }
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        query = nullptr;
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (PdhAddEnglishCounterW(query, binds[i].path, 0, binds[i].slot) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            for (int j = 0; j < count; ++j) {
                if (binds[j].slot) *binds[j].slot = nullptr;
            }
            return false;
        }
    }
    return true;
}

// QPC measures each data source at its own collection boundary, independent of
// WM_TIMER delivery, refresh selection, and time spent collecting other metrics.
std::int64_t QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double QpcSeconds(std::int64_t now, std::int64_t before) {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ?
            static_cast<double>(value.QuadPart) : 0.0;
    }();
    return frequency > 0 && now > before ? static_cast<double>(now - before) / frequency : 0.0;
}

struct EnergyPoint {
    std::wstring name;
    uint64_t pwh = 0;
    std::int64_t qpc = 0;
    uint64_t meter_time = 0; // Raw freshness marker; never used as the power denominator.
    bool have_meter_time = false, aligned = false;
};

// Domain identity is kept with the raw baseline: reordering/renaming instances
// must never subtract energy reported by a different meter.
int EnergyScore(const wchar_t* name, std::size_t domain) {
    const wchar_t* tokens[] = {L"package0_pkg", L"pp0", L"pp1", L"dram"};
    return MatchScore(name, tokens[domain]);
}

bool RawArray(PDH_HCOUNTER counter, std::vector<unsigned char>& buffer, DWORD& count) {
    if (!counter) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD need = static_cast<DWORD>(buffer.size());
        count = 0;
        const PDH_STATUS status = PdhGetRawCounterArrayW(counter, &need, &count,
            reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(buffer.data()));
        if (status == ERROR_SUCCESS) return true;
        if (static_cast<DWORD>(status) != static_cast<DWORD>(PDH_MORE_DATA) ||
            need == 0 || need > (16u << 20)) return false;
        buffer.resize(std::max(static_cast<std::size_t>(need), buffer.size() * 2));
    }
    return false;
}

struct GpuBucket {
    std::wstring adapter, engine;
    double usage = 0;
};

bool GpuIdentity(const wchar_t* name, std::wstring& adapter, std::wstring& engine) {
    if (!name) return false;
    const std::wstring instance(name);
    const auto luid = instance.find(L"luid_");
    const auto type = instance.find(L"_engtype_");
    if (luid == std::wstring::npos || type == std::wstring::npos) return false;
    const auto middle = instance.find(L"_", luid + 5);
    if (middle == std::wstring::npos) return false;
    const auto end = instance.find(L"_", middle + 1);
    if (middle == luid + 5 || end == std::wstring::npos || end == middle + 1 || end >= type) return false;
    // Only the two hexadecimal LUID parts identify the adapter. PID, physical
    // node and engine number are deliberately excluded from this grouping key.
    adapter = instance.substr(luid, end - luid);
    engine = instance.substr(type + 9);
    const auto suffix = engine.find(L'#'); // PDH duplicate-instance suffix
    if (suffix != std::wstring::npos) engine.resize(suffix);
    if (engine.empty()) return false;
    for (auto& c : adapter) c = LowerChar(c);
    for (auto& c : engine) c = LowerChar(c);
    return true;
}

uint64_t FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

uint16_t ReadU16(const unsigned char* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t ReadU32(const unsigned char* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t ReadU64(const unsigned char* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::int64_t ReadI64(const unsigned char* p) {
    std::int64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

using NtQueryFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

struct ProcPoint {
    uint64_t pid = 0;
    uint64_t ppid = 0;
    std::int64_t cpu = 0;
    std::int64_t created = 0;
    std::wstring name;
};

class ProcScan {
public:
    ProcScan() {
        proc_buf_.resize(1u << 20);
        cur_.reserve(512);
        prev_.reserve(512);
        aggs_.reserve(512);
        tree_.reserve(16);
        stack_.reserve(16);
        self_pid_ = GetCurrentProcessId();
        ncpu_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (ncpu_ == 0) {
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            ncpu_ = info.dwNumberOfProcessors;
        }
        if (ncpu_ == 0) ncpu_ = 1;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            nt_ = reinterpret_cast<NtQueryFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        }
    }

    void Reset() {
        have_ = false;
        prev_.clear();
    }

    void Release();

    // The first call only stores a baseline and returns false. The NT buffer is reused.
    // process_count excludes Idle (PID 0) and is set whenever the snapshot is read.
    bool Scan(std::vector<ProcUsage>& top, double& self_pct, int& process_count) {
        if (!Read(cur_)) { Reset(); return false; }
        const std::int64_t now = QpcNow();
        int processes = 0;
        for (const ProcPoint& proc : cur_) {
            if (proc.pid != 0) ++processes;
        }
        process_count = processes;
        std::sort(cur_.begin(), cur_.end(), [](const ProcPoint& a, const ProcPoint& b) { return a.pid < b.pid; });
        if (!have_) {
            prev_.swap(cur_);
            tick_ = now;
            have_ = true;
            return false;
        }
        const double dt = QpcSeconds(now, tick_);
        if (!(dt > 0.0)) return false;
        const double total = dt * 1.0e7 * static_cast<double>(ncpu_);
        if (!(total > 0.0)) return false;

        auto& tree = tree_;
        auto& stack = stack_;
        tree.clear();
        stack.clear();
        stack.push_back(self_pid_);
        while (!stack.empty() && tree.size() <= cur_.size()) {
            const uint64_t id = stack.back();
            stack.pop_back();
            bool seen = false;
            for (uint64_t have : tree) {
                if (have == id) {
                    seen = true;
                    break;
                }
            }
            if (seen) continue;
            tree.push_back(id);
            for (const ProcPoint& proc : cur_) {
                if (proc.ppid == id && proc.pid != id) stack.push_back(proc.pid);
            }
        }

        aggs_.clear();
        double self_ticks = 0;
        std::size_t old_index = 0;
        // Both snapshots are sorted by PID: a linear merge replaces the former
        // per-process scan of the entire previous snapshot.
        for (const ProcPoint& proc : cur_) {
            if (proc.pid == 0) continue;
            while (old_index < prev_.size() && prev_[old_index].pid < proc.pid) ++old_index;
            if (old_index == prev_.size()) break;
            const ProcPoint& old = prev_[old_index];
            if (old.pid != proc.pid || old.created != proc.created || old.name != proc.name) continue;
            const std::int64_t delta = proc.cpu - old.cpu;
            if (delta < 0) continue;
            if (std::find(tree.begin(), tree.end(), proc.pid) != tree.end()) {
                self_ticks += static_cast<double>(delta);
            } else if (!proc.name.empty()) {
                aggs_.push_back({&proc.name, static_cast<double>(delta)});
            }
        }
        // Aggregate identical executable names without allocating/copying keys.
        std::sort(aggs_.begin(), aggs_.end(), [](const Agg& a, const Agg& b) { return *a.name < *b.name; });
        std::size_t used = 0;
        for (std::size_t i = 0; i < aggs_.size(); ++i) {
            if (used && *aggs_[used - 1].name == *aggs_[i].name) aggs_[used - 1].ticks += aggs_[i].ticks;
            else aggs_[used++] = aggs_[i];
        }
        aggs_.resize(used);
        const std::size_t n = std::min(std::size_t(3), used);
        std::partial_sort(aggs_.begin(), aggs_.begin() + n, aggs_.end(), [](const Agg& a, const Agg& b) {
            return a.ticks != b.ticks ? a.ticks > b.ticks : *a.name < *b.name;
        });
        top.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            top[i].name = *aggs_[i].name;
            top[i].cpu_percent = std::clamp(aggs_[i].ticks / total * 100.0, 0.0, 100.0);
        }
        self_pct = std::clamp(self_ticks / total * 100.0, 0.0, 100.0);
        prev_.swap(cur_);
        tick_ = now;
        return true;
    }

    // prev_ is sorted by pid after Scan, including the baseline pass. Empty when
    // process collection is off or the snapshot could not be read.
    bool LookupName(std::uint64_t pid, std::wstring& out) const {
        if (!have_ || pid == 0) return false;
        const auto it = std::lower_bound(prev_.begin(), prev_.end(), pid,
            [](const ProcPoint& row, std::uint64_t id) { return row.pid < id; });
        if (it == prev_.end() || it->pid != pid || it->name.empty()) return false;
        out = it->name;
        return true;
    }

private:
    bool Read(std::vector<ProcPoint>& out) {
        if (!nt_) return false;
        if (proc_buf_.size() < (1u << 20)) proc_buf_.resize(1u << 20);
        for (int attempt = 0; attempt < 8; ++attempt) {
            ULONG ret = 0;
            const LONG raw = nt_(5, proc_buf_.data(), static_cast<ULONG>(proc_buf_.size()), &ret);
            const unsigned long status = static_cast<unsigned long>(raw);
            if (status == 0xC0000004UL) {
                std::size_t need = static_cast<std::size_t>(ret) + (64u * 1024u);
                if (need < proc_buf_.size() * 2) need = proc_buf_.size() * 2;
                if (need > (32u << 20)) return false;
                proc_buf_.resize(need);
                continue;
            }
            if (status != 0 || ret == 0) return false;
            return Parse(proc_buf_.data(), ret, out);
        }
        return false;
    }

    static bool Parse(const unsigned char* buf, std::size_t len, std::vector<ProcPoint>& out) {
        std::size_t off = 0;
        std::size_t count = 0;
        for (int guard = 0; guard < 200000; ++guard) {
            if (off + 96 > len) return false;
            const uint32_t next = ReadU32(buf + off);
            const std::int64_t user = ReadI64(buf + off + 40);
            const std::int64_t kern = ReadI64(buf + off + 48);
            const uint16_t nlen = ReadU16(buf + off + 56);
            const uint64_t ptr = ReadU64(buf + off + 64);
            if (count == out.size()) out.emplace_back();
            ProcPoint& point = out[count++];
            point.pid = ReadU64(buf + off + 80);
            point.ppid = ReadU64(buf + off + 88);
            point.cpu = user + kern;
            point.created = ReadI64(buf + off + 32);
            const auto base = reinterpret_cast<uintptr_t>(buf);
            if (ptr >= base && ptr <= base + len && nlen <= base + len - ptr && nlen <= 1024) {
                point.name.assign(reinterpret_cast<const wchar_t*>(static_cast<uintptr_t>(ptr)),
                                  nlen / sizeof(wchar_t));
                if (point.name.size() >= 4) {
                    const auto pos = point.name.size() - 4;
                    const auto* tail = point.name.c_str() + pos;
                    if (tail[0] == L'.' && LowerChar(tail[1]) == L'e' &&
                        LowerChar(tail[2]) == L'x' && LowerChar(tail[3]) == L'e') point.name.resize(pos);
                }
            } else point.name.clear();
            if (next == 0) { out.resize(count); return true; }
            if (next < 96 || off + next < off || off + next > len) return false;
            off += next;
        }
        return false;
    }

    struct Agg { const std::wstring* name; double ticks; };
    std::vector<Agg> aggs_;
    std::vector<uint64_t> tree_, stack_;
    std::vector<unsigned char> proc_buf_;
    std::vector<ProcPoint> cur_;
    std::vector<ProcPoint> prev_;
    NtQueryFn nt_ = nullptr;
    uint64_t self_pid_ = 0;
    DWORD ncpu_ = 1;
    bool have_ = false;
    std::int64_t tick_ = 0;
};

void ProcScan::Release() {
    Reset();
    std::vector<unsigned char>().swap(proc_buf_);
    std::vector<ProcPoint>().swap(cur_);
    std::vector<ProcPoint>().swap(prev_);
    std::vector<Agg>().swap(aggs_);
    std::vector<uint64_t>().swap(tree_);
    std::vector<uint64_t>().swap(stack_);
}

constexpr unsigned PackWants(const Wants& w) {
    unsigned bits = 0;
    if (w.chip_power) bits |= kWantChip;
    if (w.gpu_usage) bits |= kWantGpu;
    if (w.processes) bits |= kWantProc;
    if (w.memory_detail) bits |= kWantMemDetail;
    if (w.disk_io) bits |= kWantDisk;
    if (w.sys_disk) bits |= kWantSysDisk;
    if (w.cpu_freq) bits |= kWantCpuFreq;
    if (w.uptime) bits |= kWantUptime;
    if (w.gpu_memory) bits |= kWantGpuMem;
    if (w.wifi) bits |= kWantWifi;
    if (w.battery_health) bits |= kWantBattery;
    if (w.fps) bits |= kWantFps;
    if (w.power_scheme) bits |= kWantScheme;
    if (w.process_count) bits |= kWantCount;
    if (w.gpu_temp) bits |= kWantGpuTemp;
    if (w.dgpu_power) bits |= kWantDgpuPower;
    return bits;
}

constexpr Wants UnpackWants(unsigned bits) {
    Wants w;
    w.chip_power = (bits & kWantChip) != 0;
    w.gpu_usage = (bits & kWantGpu) != 0;
    w.processes = (bits & kWantProc) != 0;
    w.memory_detail = (bits & kWantMemDetail) != 0;
    w.disk_io = (bits & kWantDisk) != 0;
    w.sys_disk = (bits & kWantSysDisk) != 0;
    w.cpu_freq = (bits & kWantCpuFreq) != 0;
    w.uptime = (bits & kWantUptime) != 0;
    w.gpu_memory = (bits & kWantGpuMem) != 0;
    w.wifi = (bits & kWantWifi) != 0;
    w.battery_health = (bits & kWantBattery) != 0;
    w.fps = (bits & kWantFps) != 0;
    w.power_scheme = (bits & kWantScheme) != 0;
    w.process_count = (bits & kWantCount) != 0;
    w.gpu_temp = (bits & kWantGpuTemp) != 0;
    w.dgpu_power = (bits & kWantDgpuPower) != 0;
    return w;
}

static_assert(PackWants(Wants{}) == kWantDefault, "default Wants");

constexpr Wants FpsWantOnly() {
    Wants wants{};
    wants.chip_power = false;
    wants.gpu_usage = false;
    wants.processes = false;
    wants.fps = true;
    return wants;
}
static_assert(PackWants(FpsWantOnly()) == kWantFps, "fps want bit");
static_assert(UnpackWants(kWantFps).fps, "unpack fps");
static_assert(!UnpackWants(kWantDefault).fps, "default fps off");
static_assert(UnpackWants(kWantGpuTemp).gpu_temp, "unpack gpu temperature");
static_assert(PackWants(UnpackWants(kWantGpuTemp)) == kWantGpuTemp, "gpu temperature want bit");
static_assert(!UnpackWants(kWantDefault).gpu_temp, "default gpu temperature off");

static_assert(UnpackWants(kWantDgpuPower).dgpu_power, "unpack dgpu power");
static_assert(PackWants(UnpackWants(kWantDgpuPower)) == kWantDgpuPower, "dgpu power want bit");
static_assert(!UnpackWants(kWantDefault).dgpu_power, "default dgpu power off");

using WlanOpenHandleFn = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
using WlanCloseHandleFn = DWORD(WINAPI*)(HANDLE, PVOID);
using WlanEnumInterfacesFn = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
using WlanQueryInterfaceFn = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*,
                                           PWLAN_OPCODE_VALUE_TYPE);
using WlanFreeMemoryFn = VOID(WINAPI*)(PVOID);

std::wstring SsidToWide(const UCHAR* data, ULONG len) {
    if (!data || len == 0) return L"";
    if (len > DOT11_SSID_MAX_LENGTH) len = DOT11_SSID_MAX_LENGTH;
    bool ascii = true;
    for (ULONG i = 0; i < len; ++i) {
        if (data[i] < 0x20 || data[i] > 0x7E) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        std::wstring out(len, L'\0');
        for (ULONG i = 0; i < len; ++i) out[i] = static_cast<wchar_t>(data[i]);
        return out;
    }
    const char* bytes = reinterpret_cast<const char*>(data);
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(len), nullptr, 0);
    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {
        codepage = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(CP_ACP, 0, bytes, static_cast<int>(len), nullptr, 0);
    }
    if (n <= 0) return L"";
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(codepage, flags, bytes, static_cast<int>(len), out.data(), n);
    return out;
}

// Capacities may be mWh or relative units; the ratio is valid either way.
bool QueryBatteryDevice(HANDLE device, double& health, int& cycles, unsigned long& designed) {
    DWORD wait = 0;
    DWORD tag = 0;
    DWORD got = 0;
    if (!DeviceIoControl(device, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag), &got, nullptr) ||
        tag == 0) {
        return false;
    }
    BATTERY_QUERY_INFORMATION query{};
    query.BatteryTag = tag;
    query.InformationLevel = BatteryInformation;
    BATTERY_INFORMATION info{};
    got = 0;
    if (!DeviceIoControl(device, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &info, sizeof(info), &got,
                         nullptr)) {
        return false;
    }
    if (got < offsetof(BATTERY_INFORMATION, DefaultAlert1)) return false;
    constexpr unsigned long kUnknown = 0xFFFFFFFFu;
    if (info.DesignedCapacity == 0 || info.DesignedCapacity == kUnknown) return false;
    if (info.FullChargedCapacity == 0 || info.FullChargedCapacity == kUnknown) return false;
    const double pct = 100.0 * static_cast<double>(info.FullChargedCapacity) /
                       static_cast<double>(info.DesignedCapacity);
    if (!std::isfinite(pct) || pct <= 0.0 || pct > 200.0) return false;
    health = pct;
    designed = info.DesignedCapacity;
    if (got >= sizeof(BATTERY_INFORMATION) && info.CycleCount > 0 && info.CycleCount != kUnknown) {
        const unsigned long capped = info.CycleCount > 1000000ul ? 1000000ul : info.CycleCount;
        cycles = static_cast<int>(capped);
    } else {
        cycles = -1;
    }
    return true;
}

// GUID_DEVCLASS_BATTERY {72631e54-78a4-11d0-bcf7-00aa00b7b32a}. The largest designed
// capacity wins when several battery devices are present.
bool ReadBatteryHealth(double& health, int& cycles) {
    struct SetupApi {
        HMODULE module = LoadLibraryExW(L"setupapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&SetupDiGetClassDevsW) get = nullptr;
        decltype(&SetupDiEnumDeviceInterfaces) enumerate = nullptr;
        decltype(&SetupDiGetDeviceInterfaceDetailW) detail = nullptr;
        decltype(&SetupDiDestroyDeviceInfoList) destroy = nullptr;
        SetupApi() {
            if (!module) return;
            get = reinterpret_cast<decltype(get)>(GetProcAddress(module, "SetupDiGetClassDevsW"));
            enumerate = reinterpret_cast<decltype(enumerate)>(GetProcAddress(module, "SetupDiEnumDeviceInterfaces"));
            detail = reinterpret_cast<decltype(detail)>(GetProcAddress(module, "SetupDiGetDeviceInterfaceDetailW"));
            destroy = reinterpret_cast<decltype(destroy)>(GetProcAddress(module, "SetupDiDestroyDeviceInfoList"));
        }
        ~SetupApi() { if (module) FreeLibrary(module); }
    };
    static const SetupApi api;
    if (!api.get || !api.enumerate || !api.detail || !api.destroy) return false;
    const GUID guid = MakeGuid(0x72631e54, 0x78A4, 0x11d0, 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a);
    const HDEVINFO devices = api.get(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    double best_health = NAN;
    int best_cycles = -1;
    unsigned long best_designed = 0;
    for (DWORD index = 0; index < 32; ++index) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!api.enumerate(devices, nullptr, &guid, index, &iface)) break;
        DWORD need = 0;
        api.detail(devices, &iface, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || need > (64u * 1024u)) continue;
        std::vector<unsigned char> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        // x64 requires 8. sizeof() is 6 on some MinGW headers, which SetupDi rejects.
        detail->cbSize = 8;
        if (!api.detail(devices, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            device = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (device == INVALID_HANDLE_VALUE) continue;
        double one_health = NAN;
        int one_cycles = -1;
        unsigned long designed = 0;
        const bool ok = QueryBatteryDevice(device, one_health, one_cycles, designed);
        CloseHandle(device);
        if (!ok) continue;
        if (!found || designed >= best_designed) {
            found = true;
            best_designed = designed;
            best_health = one_health;
            best_cycles = one_cycles;
        }
    }
    api.destroy(devices);
    if (!found) return false;
    health = best_health;
    cycles = best_cycles;
    return true;
}

void StripExeSuffix(std::wstring& name) {
    if (name.size() < 4) return;
    const std::size_t pos = name.size() - 4;
    const wchar_t* tail = name.c_str() + pos;
    if (tail[0] == L'.' && LowerChar(tail[1]) == L'e' && LowerChar(tail[2]) == L'x' &&
        LowerChar(tail[3]) == L'e') {
        name.resize(pos);
    }
}

std::wstring QueryImageBaseName(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";
    std::wstring path(512, L'\0');
    bool ok = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, &path[0], &size)) {
            path.resize(size);
            ok = true;
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size <= 1 || size > 32768) break;
        path.assign(size, L'\0');
    }
    CloseHandle(process);
    if (!ok) return L"";
    const auto slash = path.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
    StripExeSuffix(base);
    return base;
}

bool ProcessStillExists(DWORD pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        CloseHandle(process);
        return true;
    }
    // A reused or exited pid fails with ERROR_INVALID_PARAMETER. Access denied still exists.
    return GetLastError() != ERROR_INVALID_PARAMETER;
}

bool IsDesktopOrTaskbar(HWND hwnd) {
    if (!hwnd) return true;
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) return true;
    wchar_t cls[64];
    cls[0] = 0;
    if (GetClassNameW(hwnd, cls, 64) <= 0) return false;
    const wchar_t* ignored[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                L"NotifyIconOverflowWindow"};
    for (const wchar_t* name : ignored) {
        if (std::wcscmp(cls, name) == 0) return true;
    }
    return false;
}

struct FpsWindowCandidate { DWORD pid = 0; };

BOOL CALLBACK FindFpsApplication(HWND hwnd, LPARAM context) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsDesktopOrTaskbar(hwnd)) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    // A second Pulse instance or one of its cards is not a render target either.
    if (std::wcscmp(cls, L"Pulse.Native.Main.v1") == 0 ||
        std::wcscmp(cls, L"Pulse.Native.Notification.v1") == 0) return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;
    reinterpret_cast<FpsWindowCandidate*>(context)->pid = pid;
    return FALSE;
}

// Clicking this widget, the desktop, or the taskbar keeps a live previous target.
// An elevated/new instance has no previous pid. When Pulse owns the foreground,
// seed it from the next visible application in Z order instead of staying empty.
DWORD TrackedForegroundPid(DWORD previous) {
    const HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    if (!IsDesktopOrTaskbar(hwnd) && pid != 0 && pid != GetCurrentProcessId()) return pid;
    if (ProcessStillExists(previous)) return previous;
    if (pid == GetCurrentProcessId()) {
        FpsWindowCandidate candidate;
        EnumWindows(FindFpsApplication, reinterpret_cast<LPARAM>(&candidate));
        return candidate.pid;
    }
    return 0;
}

// Parent links for the foreground Present rate. Toolhelp is not the process-CPU
// scan: that scan is off in compact mode, and FPS must still see GPU children.
// The snapshot is refreshed every few seconds, or immediately when the foreground
// process changes. Sample walks this cache; it does not snapshot every tick.
class FpsTree {
public:
    bool idle() const { return at_ == 0 && rows_.empty(); }
    bool Due(unsigned long long now, bool root_changed) const {
        if (root_changed || at_ == 0) return true;
        return now - at_ >= 3000ull;
    }
    void Clear() {
        std::vector<Row>().swap(rows_);
        std::vector<Child>().swap(children_);
        at_ = 0;
    }
    void Refresh(unsigned long long now);
    bool Lookup(DWORD pid, std::wstring& name) const;
    void Collect(DWORD root, std::vector<DWORD>& out) const;

private:
    struct Row {
        DWORD pid = 0;
        DWORD ppid = 0;
        std::wstring name;
    };
    struct Child {
        DWORD parent = 0;
        DWORD child = 0;
    };
    std::vector<Row> rows_;
    std::vector<Child> children_;
    unsigned long long at_ = 0;
};

void FpsTree::Refresh(unsigned long long now) {
    at_ = now;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::vector<Row> rows;
    rows.reserve(rows_.empty() ? 256 : rows_.size());
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            Row row;
            row.pid = entry.th32ProcessID;
            row.ppid = entry.th32ParentProcessID;
            if (row.pid != 0) {
                row.name.assign(entry.szExeFile);
                StripExeSuffix(row.name);
                rows.push_back(std::move(row));
            }
            entry.dwSize = sizeof(entry);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    if (rows.empty()) return;
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pid < b.pid; });
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [](const Row& a, const Row& b) { return a.pid == b.pid; }),
               rows.end());
    std::vector<Child> children;
    children.reserve(rows.size());
    for (const Row& row : rows) {
        if (row.ppid == row.pid) continue;
        Child link;
        link.parent = row.ppid;
        link.child = row.pid;
        children.push_back(link);
    }
    std::sort(children.begin(), children.end(), [](const Child& a, const Child& b) {
        return a.parent < b.parent || (a.parent == b.parent && a.child < b.child);
    });
    rows_.swap(rows);
    children_.swap(children);
}

bool FpsTree::Lookup(DWORD pid, std::wstring& name) const {
    if (pid == 0 || rows_.empty()) return false;
    const auto it = std::lower_bound(rows_.begin(), rows_.end(), pid,
        [](const Row& row, DWORD key) { return row.pid < key; });
    if (it == rows_.end() || it->pid != pid || it->name.empty()) return false;
    name = it->name;
    return true;
}

void FpsTree::Collect(DWORD root, std::vector<DWORD>& out) const {
    out.clear();
    if (root == 0) return;
    out.push_back(root);
    // Direct children are stored in pid order, so a browser's early GPU process
    // is kept even when a later cap drops extra renderers.
    for (std::size_t i = 0; i < out.size() && out.size() < 512; ++i) {
        Child key;
        key.parent = out[i];
        key.child = 0;
        const auto range = std::equal_range(children_.begin(), children_.end(), key,
            [](const Child& a, const Child& b) { return a.parent < b.parent; });
        for (auto it = range.first; it != range.second && out.size() < 512; ++it) {
            const DWORD child = it->child;
            if (child == 0 || child == out[i]) continue;
            bool seen = false;
            for (DWORD id : out) {
                if (id == child) {
                    seen = true;
                    break;
                }
            }
            if (!seen) out.push_back(child);
        }
    }
}

}  // namespace

bool detail::AllPhysicalNicsDisconnected() {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return false;
    bool disconnected = true;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        if (!IsPhysicalNic(table->Table[i])) continue;
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = table->Table[i].InterfaceLuid;
        // Re-read current link state, including media disconnects on an Up row.
        // A vanished/unreadable NIC is uncertain; let HTTP confirmation handle it.
        if (GetIfEntry2(&row) != NO_ERROR ||
            (row.OperStatus == IfOperStatusUp &&
             row.MediaConnectState != MediaConnectStateDisconnected)) {
            disconnected = false;
            break;
        }
    }
    FreeMibTable(table);
    return disconnected;
}

struct Metrics::Impl {
    Impl() {
        last_.cpu_percent = NAN; // No fabricated zero while both CPU sources warm up.
        if (NotifyIpInterfaceChange(AF_UNSPEC, NicChanged, this, FALSE, &nic_notify_) != NO_ERROR)
            nic_notify_ = nullptr;
    }
    ~Impl() {
        if (nic_notify_) CancelMibChangeNotify2(nic_notify_);
        probe.Stop();
        CloseQueries();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Snapshot Take(bool full);
    void ApplyWants(const Wants& wants) { want_bits_.store(PackWants(wants), std::memory_order_release); }

    NetProbe probe;
    std::atomic<bool> paused_{false};
    std::atomic<bool> reset_intervals_{false};
    Snapshot last_{};

    bool detail_ = false;
    bool have_tick_ = false;
    unsigned long long last_tick_ = 0;

    bool have_bat_ = false;
    BatteryReading bat_{};
    bool prev_valid_ = false;
    PowerMode prev_mode_ = PowerMode::Unknown;
    bool prev_ok_ = false;
    bool prev_ac_known_ = false;
    bool prev_ac_online_ = false;
    bool prev_contested_ = false;
    bool prev_pure_ = false;
    AcDischargeDebounce ac_discharge_;
    bool have_ema_ = false;
    double ema_ = 0;
    unsigned long long unplug_tick_ = 0;
    bool since_start_ = false;
    double energy_wh_ = 0;
    double measured_s_ = 0;

    std::wstring scheme_;
    PowerSchemeKind scheme_kind_ = PowerSchemeKind::Unknown;
    bool scheme_ready_ = false;
    unsigned long long scheme_tick_ = 0;
    OverlayFn overlay_fn_ = nullptr;
    ActiveSchemeFn active_fn_ = nullptr;
    FriendlyNameFn friendly_fn_ = nullptr;
    bool power_api_tried_ = false;
    GUID active_guid_{};
    bool have_active_guid_ = false;
    bool active_valid_ = false;
    std::wstring active_name_;
    unsigned long long active_name_tick_ = 0;

    static void CALLBACK NicChanged(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
        auto* self = static_cast<Impl*>(context);
        self->nics_dirty_.store(true, std::memory_order_relaxed);
        self->probe.RequestProbe();
    }
    HANDLE nic_notify_ = nullptr;
    std::atomic<bool> nics_dirty_{true};
    std::vector<ULONG> found_nics_;
    bool nics_ready_ = false;
    unsigned long long nics_tick_ = 0;
    std::vector<ULONG> nics_;
    std::vector<ULONG> prev_nics_;
    bool have_net_ = false;
    uint64_t prev_rx_ = 0;
    uint64_t prev_tx_ = 0;
    std::int64_t net_qpc_ = 0;
    uint64_t rx_total_ = 0;
    uint64_t tx_total_ = 0;

    bool have_cpu_ = false;
    uint64_t prev_idle_ = 0;
    uint64_t prev_total_ = 0;

    // One persistent query; optional frequency counters join/leave it without
    // disturbing Utility. Compact mode keeps only the single Utility counter.
    PDH_HQUERY cpu_query_ = nullptr;
    PDH_HCOUNTER cpu_utility_ = nullptr;
    bool cpu_utility_warmed_ = false;
    bool have_cpu_query_tick_ = false;
    unsigned long long cpu_query_tick_ = 0;
    unsigned long long cpu_query_retry_at_ = 0, cpu_utility_retry_at_ = 0;

    ProcScan procs_;
    std::vector<ProcUsage> top_cache_;
    double self_cache_ = NAN;
    unsigned long long procs_due_ = 0;

    bool have_energy_due_ = false;
    unsigned long long energy_due_ = 0;
    bool energy_ok_ = false;
    unsigned long long energy_retry_at_ = 0;
    PDH_HQUERY energy_query_ = nullptr;
    PDH_HCOUNTER energy_counter_ = nullptr;
    PDH_HCOUNTER power_counter_ = nullptr;
    PDH_HCOUNTER energy_time_counter_ = nullptr;
    std::array<EnergyPoint, 4> energy_prev_{};
    bool pkg_from_energy_ = false;
    double pkg_delta_pwh_ = NAN, pkg_interval_s_ = NAN;
    double pkg_power_counter_w_ = NAN;
    double pkg_w_ = NAN;
    double cpu_w_ = NAN;
    double gpu_w_ = NAN;
    double mem_w_ = NAN;
    std::array<MeterStatus, 4> meter_status_{};

    bool gpu_ok_ = false;
    bool gpu_warmed_ = false;
    unsigned long long gpu_retry_at_ = 0;
    unsigned long long gpu_due_ = 0;
    PDH_HQUERY gpu_query_ = nullptr;
    PDH_HCOUNTER gpu_counter_ = nullptr;
    double gpu_percent_ = NAN;
    std::vector<GpuBucket> gpu_buckets_;

    std::atomic<unsigned> want_bits_{kWantDefault};
    bool procs_buffers_ = true;
    int process_count_ = -1;

    bool disk_ok_ = false;
    bool disk_warmed_ = false;
    bool have_disk_due_ = false;
    unsigned long long disk_due_ = 0;
    unsigned long long disk_retry_at_ = 0;
    PDH_HQUERY disk_query_ = nullptr;
    PDH_HCOUNTER disk_read_counter_ = nullptr;
    PDH_HCOUNTER disk_write_counter_ = nullptr;
    double disk_read_ = NAN;
    double disk_write_ = NAN;

    bool cpu_freq_ok_ = false;
    bool cpu_freq_warmed_ = false;
    unsigned long long cpu_freq_retry_at_ = 0;
    PDH_HCOUNTER cpu_freq_perf_ = nullptr;
    PDH_HCOUNTER cpu_freq_base_ = nullptr;
    double cpu_mhz_ = NAN;

    bool have_sys_due_ = false;
    unsigned long long sys_due_ = 0;
    bool sys_root_ready_ = false;
    wchar_t sys_root_[4] = {};
    double sys_used_pct_ = NAN;
    double sys_free_gb_ = NAN;

    bool gpu_mem_ok_ = false;
    bool have_gpu_mem_due_ = false;
    unsigned long long gpu_mem_due_ = 0;
    unsigned long long gpu_mem_retry_at_ = 0;
    PDH_HQUERY gpu_mem_query_ = nullptr;
    PDH_HCOUNTER gpu_mem_shared_ = nullptr;
    PDH_HCOUNTER gpu_mem_dedicated_ = nullptr;
    double gpu_shared_mb_ = NAN;
    double gpu_dedicated_mb_ = NAN;

    DgpuPower dgpu_;

    PulseKmtApi gpu_temp_api_;
    std::vector<PulseKmtAdapterInfo> gpu_temp_adapters_;
    bool gpu_temp_ready_ = false, have_gpu_temp_due_ = false;
    bool gpu_temp_seen_ = false;
    unsigned long long gpu_temp_due_ = 0, gpu_temp_retry_at_ = 0, gpu_temp_enum_at_ = 0;
    double gpu_temp_c_ = NAN;

    HMODULE wifi_mod_ = nullptr;
    HANDLE wifi_client_ = nullptr;
    WlanOpenHandleFn wifi_open_ = nullptr;
    WlanCloseHandleFn wifi_close_ = nullptr;
    WlanEnumInterfacesFn wifi_enum_ = nullptr;
    WlanQueryInterfaceFn wifi_query_ = nullptr;
    WlanFreeMemoryFn wifi_free_ = nullptr;
    bool wifi_tried_ = false;
    bool have_wifi_due_ = false;
    unsigned long long wifi_due_ = 0;
    unsigned long long wifi_retry_at_ = 0;
    std::wstring wifi_ssid_;
    int wifi_signal_ = -1;

    bool have_battery_due_ = false;
    unsigned long long battery_due_ = 0;
    double battery_health_ = NAN;
    int battery_cycles_ = -1;

    FpsMonitor fps_mon_;
    bool fps_attempted_ = false;
    bool fps_ok_ = false;
    unsigned long long fps_retry_at_ = 0;
    DWORD fps_pid_ = 0;
    std::wstring fps_name_;
    FpsTree fps_tree_;
    std::vector<DWORD> fps_family_;
    int fps_logged_status_ = 0;
    bool fps_logged_ready_ = false;

    std::vector<unsigned char> pdh_buf_ = std::vector<unsigned char>(65536);

private:
    void CloseQueries();
    void ReleaseEnergy();
    void ReleaseGpu();
    void ReleaseProcs();
    void ReleaseDisk();
    void ReleaseCpuQuery();
    void ReleaseCpuFreq();
    void ReleaseGpuMem();
    void ReleaseGpuTemp();
    void ReleaseWifi();
    void ReleaseSysDisk();
    void ReleaseBatteryHealth();
    void ReleaseFps(const wchar_t* reason = L"disabled");
    void ClearTurnedOff(Snapshot& s, const Wants& wants);
    Wants CurrentWants() const;

    void UpdateNet(Snapshot& s, unsigned long long now);
    void UpdateCpu(Snapshot& s, unsigned long long now, bool frequency);
    void UpdateMem(Snapshot& s, bool detail);
    void UpdateBattery(Snapshot& s, unsigned long long now, double dt, bool has_dt);
    void UpdateScheme(unsigned long long now);
    void UpdateActivePlan(unsigned long long now);
    void LoadPowerApi();
    void UpdateEnergy(bool full, unsigned long long now);
    void UpdateGpu(bool full, unsigned long long now);
    void UpdateProcs(Snapshot& s, unsigned long long now);
    void UpdateDisk(unsigned long long now, Snapshot& s);
    void UpdateSysDisk(unsigned long long now, Snapshot& s);
    void UpdateCpuFreq(Snapshot& s);
    void UpdateUptime(Snapshot& s);
    void UpdateGpuMemory(unsigned long long now, Snapshot& s);
    void UpdateGpuTemp(unsigned long long now, Snapshot& s);
    void UpdateWifi(unsigned long long now, Snapshot& s);
    void UpdateBatteryHealth(unsigned long long now, Snapshot& s);
    void UpdateFps(Snapshot& s, unsigned long long now);
    bool EnsureEnergy(bool full, unsigned long long now);
    bool EnsureGpu(unsigned long long now);
    bool EnsureDisk(unsigned long long now);
    bool EnsureCpuQuery(unsigned long long now);
    bool EnsureCpuUtility(unsigned long long now);
    bool EnsureCpuFreq(unsigned long long now);
    bool EnsureGpuMem(unsigned long long now);
    bool EnsureGpuTemp(unsigned long long now);
    bool EnsureWifi(unsigned long long now);
    bool ReadEnergy(bool full);
    bool ReadGpu(double& out);
    PdhRead ReadDisk(double& read_bps, double& write_bps);
    PdhRead ReadCpuFreq(double& mhz);
    bool ReadGpuMemory(double& shared_mb, double& dedicated_mb);
    bool ReadWifi(std::wstring& ssid, int& signal);
    bool CopyInstances(PDH_HCOUNTER counter, std::vector<std::pair<std::wstring, double>>& out);
    bool DiscoverNics(std::vector<ULONG>& out) const;
    OverlayFn Overlay();
};

void Metrics::Impl::LoadPowerApi() {
    if (power_api_tried_) return;
    power_api_tried_ = true;
    HMODULE mod = GetModuleHandleW(L"powrprof.dll");
    if (!mod) mod = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) return;
    overlay_fn_ = reinterpret_cast<OverlayFn>(GetProcAddress(mod, "PowerGetEffectiveOverlayScheme"));
    active_fn_ = reinterpret_cast<ActiveSchemeFn>(GetProcAddress(mod, "PowerGetActiveScheme"));
    friendly_fn_ = reinterpret_cast<FriendlyNameFn>(GetProcAddress(mod, "PowerReadFriendlyName"));
}

OverlayFn Metrics::Impl::Overlay() {
    LoadPowerApi();
    return overlay_fn_;
}

bool Metrics::Impl::DiscoverNics(std::vector<ULONG>& out) const {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return false;
    out.clear();
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (IsPhysicalNic(row) && row.OperStatus == IfOperStatusUp) {
            out.push_back(row.InterfaceIndex);
        }
    }
    FreeMibTable(table);
    return true;
}

void Metrics::Impl::UpdateNet(Snapshot& s, unsigned long long now) {
    diagnostics::ScopedTiming timing(L"Network throughput");
    if (nics_dirty_.exchange(false, std::memory_order_relaxed) || !nics_ready_ || now - nics_tick_ >= kNicEveryMs) {
        diagnostics::ScopedTiming discovery(L"NIC discovery (within throughput)");
        if (DiscoverNics(found_nics_)) {
            std::sort(found_nics_.begin(), found_nics_.end());
            nics_.swap(found_nics_);
            nics_ready_ = true;
            nics_tick_ = now;
        }
    }
    uint64_t rx = 0;
    uint64_t tx = 0;
    bool complete = true;
    for (ULONG index : nics_) {
        MIB_IF_ROW2 row;
        std::memset(&row, 0, sizeof(row));
        row.InterfaceIndex = index;
        if (GetIfEntry2(&row) == NO_ERROR) {
            rx += row.InOctets;
            tx += row.OutOctets;
        } else complete = false;
    }
    const std::int64_t qpc = QpcNow();
    const double dt = QpcSeconds(qpc, net_qpc_);
    double down = complete ? 0.0 : NAN;
    double up = complete ? 0.0 : NAN;
    if (complete && have_net_ && dt > 0.0 && nics_ == prev_nics_) {
        if (rx >= prev_rx_) {
            const uint64_t delta = rx - prev_rx_;
            down = static_cast<double>(delta) / dt;
            rx_total_ += delta;
        }
        if (tx >= prev_tx_) {
            const uint64_t delta = tx - prev_tx_;
            up = static_cast<double>(delta) / dt;
            tx_total_ += delta;
        }
    }
    prev_nics_ = nics_;
    prev_rx_ = rx;
    prev_tx_ = tx;
    net_qpc_ = qpc;
    have_net_ = complete;
    s.down_bps = down;
    s.up_bps = up;
    s.rx_total = rx_total_;
    s.tx_total = tx_total_;
}

void Metrics::Impl::UpdateCpu(Snapshot& s, unsigned long long now, bool frequency) {
    diagnostics::ScopedTiming timing(L"CPU");
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    double cpu = last_.cpu_percent;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const uint64_t idle_v = FileTimeToU64(idle);
        const uint64_t total = FileTimeToU64(kernel) + FileTimeToU64(user);
        // Kernel + user is the measured elapsed time summed over logical
        // CPUs (kernel includes Idle). Its delta is the real denominator,
        // independent of the requested refresh interval; never assume 1 second.
        if (have_cpu_ && total > prev_total_ && idle_v >= prev_idle_) {
            const double idle_d = static_cast<double>(idle_v - prev_idle_);
            const double total_d = static_cast<double>(total - prev_total_);
            if (total_d > 0) {
                cpu = 100.0 * (1.0 - idle_d / total_d);
                if (cpu < 0) cpu = 0;
                if (cpu > 100.0) cpu = 100.0;
                s.cpu_time_percent = cpu;
            }
        }
        prev_idle_ = idle_v;
        prev_total_ = total;
        have_cpu_ = true;
    } else {
        have_cpu_ = false;
    }
    s.cpu_percent = cpu;

    EnsureCpuUtility(now);
    if (frequency) EnsureCpuFreq(now);
    if (!cpu_query_ || (!cpu_utility_ && !cpu_freq_ok_)) return;
    // Menu changes can request an immediate refresh beside a timer tick. Do not
    // replace a useful rate interval with two nearly simultaneous PDH samples.
    if (have_cpu_query_tick_ && now - cpu_query_tick_ < 250) {
        s.cpu_percent = last_.cpu_percent;
        s.cpu_utility_percent = last_.cpu_utility_percent;
        s.cpu_from_utility = last_.cpu_from_utility;
        if (frequency) s.cpu_mhz = cpu_mhz_;
        return;
    }
    {
        diagnostics::ScopedTiming pdh_timing(L"PDH CPU utility (shared query)");
        if (PdhCollectQueryData(cpu_query_) != ERROR_SUCCESS) {
            ReleaseCpuQuery();
            cpu_query_retry_at_ = now + kPdhRetryMs;
            return; // Keep GetSystemTimes or the last value during a PDH outage.
        }
    }
    have_cpu_query_tick_ = true;
    cpu_query_tick_ = now;
    if (cpu_utility_) {
        if (cpu_utility_warmed_) {
            double utility = NAN;
            if (FormatDouble(cpu_utility_, utility, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100)) {
                s.cpu_utility_percent = utility;
                s.cpu_from_utility = true;
                s.cpu_percent = std::clamp(utility, 0.0, 100.0);
            } else {
                cpu_utility_warmed_ = false; // Re-establish two valid samples.
            }
        } else {
            cpu_utility_warmed_ = true;
        }
    }
    if (frequency) UpdateCpuFreq(s); // Formats this same collection; no extra query read.
}

void Metrics::Impl::UpdateMem(Snapshot& s, bool detail) {
    diagnostics::ScopedTiming timing(L"Memory");
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return;
    s.mem_percent = static_cast<double>(mem.dwMemoryLoad);
    if (!detail) return;
    diagnostics::ScopedTiming detail_timing(L"Memory detail");
    const double total = static_cast<double>(mem.ullTotalPhys);
    const double avail = static_cast<double>(mem.ullAvailPhys);
    if (!(total > 0.0)) return;
    s.mem_total_gb = total / kGiB;
    s.mem_used_gb = (total > avail ? total - avail : 0.0) / kGiB;
}

void Metrics::Impl::UpdateBattery(Snapshot& s, unsigned long long now, double dt, bool has_dt) {
    diagnostics::ScopedTiming timing(L"Battery");
    BatteryReading cur{};
    const bool read_ok = ReadBattery(cur);
    // A failed read must not keep claiming the previous AC line or rate.
    // The pure-battery session itself stays put so one miss does not reset it;
    // prev_ok_ blocks that gap from entering the average. Wake has no dt.
    if (!read_ok) {
        ac_discharge_.Update(false, true, now);
        have_ema_ = false;
        ema_ = 0.0;
        prev_ok_ = false;
        s.mode = PowerMode::Unknown;
        s.percent = -1;
        s.power_trace = PowerTrace::Unknown;
        s.battery_rate_mw = NAN;
        return;
    }
    bat_ = cur;
    have_bat_ = true;
    BatterySample sample;
    sample.read_ok = true;
    sample.present = cur.present;
    sample.ac_known = cur.ac_known;
    sample.ac_online = cur.ac;
    sample.charging = cur.charging;
    sample.discharging = cur.discharging;
    sample.rate_known = cur.rate_known;
    sample.rate_mw = cur.rate_mw;
    BatteryClassification classified = ClassifyBattery(sample);
    const bool raw_supplemental = classified.supplemental;
    const bool hard_exit = !cur.present || !classified.ac_known || !classified.ac_online ||
        cur.charging || (classified.rate_valid && classified.rate_mw > 0.0);
    const bool supplemental = ac_discharge_.Update(raw_supplemental, hard_exit, now);
    if (supplemental) {
        classified.mode = PowerMode::Discharging;
        classified.supplemental = true;
        classified.pure_battery = false;
        classified.show_watts = classified.rate_valid && classified.rate_mw < 0.0;
        classified.eta_discharge = classified.eta_charge = false;
        classified.trace = PowerTrace::Package;
    } else if (raw_supplemental) {
        // Keep pending AC discharge out of watts, ETA and unplug statistics.
        classified.mode = PowerMode::AcIdle;
        classified.supplemental = false;
        classified.show_watts = false;
        classified.trace = PowerTrace::Package;
    }
    const bool session_changed = !prev_valid_ || !prev_ok_ || prev_mode_ != classified.mode ||
        prev_ac_known_ != classified.ac_known || prev_ac_online_ != classified.ac_online ||
        prev_contested_ != classified.contested || prev_pure_ != classified.pure_battery;
    const double magnitude = classified.show_watts ? std::fabs(classified.rate_mw) : 0.0;

    if (classified.show_watts && (session_changed || !have_ema_ || !has_dt || !(dt > 0.0))) {
        ema_ = magnitude;
        have_ema_ = true;
    } else if (classified.show_watts) {
        const double alpha = 1.0 - std::exp(-dt / kTauSec);
        ema_ += alpha * (magnitude - ema_);
    } else {
        have_ema_ = false;
        ema_ = 0.0;
    }

    if (classified.pure_battery) {
        if (!prev_pure_) {
            unplug_tick_ = now;
            since_start_ = !prev_valid_;
            energy_wh_ = 0.0;
            measured_s_ = 0.0;
        } else if (prev_ok_ && has_dt && dt > 0.0 && dt <= kMaxGapSec && classified.show_watts) {
            energy_wh_ += (magnitude / 1000.0) * dt / 3600.0;
            measured_s_ += dt;
        }
    } else {
        unplug_tick_ = 0;
    }

    double eta = NAN;
    double eta20 = NAN;
    double pph = NAN;
    bool from_system = false;
    if (have_ema_ && ema_ > 0.0 && cur.present) {
        if (classified.eta_discharge) {
            eta = cur.remaining_mwh / ema_ * 3600.0;
            if (cur.full_mwh > 0.0) {
                pph = ema_ / cur.full_mwh * 100.0;
                if (cur.percent > 20) {
                    const double remain = (cur.remaining_mwh - 0.2 * cur.full_mwh) / ema_ * 3600.0;
                    if (remain >= 0.0) eta20 = remain;
                }
            }
        } else if (classified.eta_charge && cur.full_mwh > cur.remaining_mwh) {
            eta = (cur.full_mwh - cur.remaining_mwh) / ema_ * 3600.0;
            if (cur.full_mwh > 0.0) pph = ema_ / cur.full_mwh * 100.0;
        }
    }
    if (UseSystemBatteryEta(classified, eta, cur.life_s)) {
        eta = cur.life_s;
        from_system = true;
    }
    if (classified.pure_battery && cur.percent >= 0 && cur.percent <= 20) eta20 = 0.0;

    s.mode = classified.mode;
    s.percent = cur.percent;
    s.power_w = classified.show_watts ? magnitude / 1000.0 : NAN;
    s.eta_s = eta;
    s.eta_from_system = from_system;
    s.eta20_s = eta20;
    s.pct_per_hour = pph;
    s.battery_saver = cur.saver;
    if (cur.present) s.remaining_wh = cur.remaining_mwh / 1000.0;
    if (cur.present && cur.full_mwh > 0.0) s.full_wh = cur.full_mwh / 1000.0;
    if (classified.pure_battery && unplug_tick_ != 0) {
        s.on_battery_s = static_cast<double>(now - unplug_tick_) / 1000.0;
        s.on_battery_since_start = since_start_;
        if (s.on_battery_s > 0.0 && measured_s_ > kAvgMinSec) {
            s.avg_w = energy_wh_ / (measured_s_ / 3600.0);
        }
    }
    s.ac_known = classified.ac_known;
    s.ac_online = classified.ac_online;
    s.battery_rate_valid = classified.rate_valid;
    s.battery_rate_mw = classified.rate_mw;
    s.battery_supplemental = classified.supplemental;
    s.power_contested = classified.contested;
    s.power_trace = classified.trace;

    prev_mode_ = classified.mode;
    prev_ac_known_ = classified.ac_known;
    prev_ac_online_ = classified.ac_online;
    prev_contested_ = classified.contested;
    prev_pure_ = classified.pure_battery;
    prev_ok_ = true;
    prev_valid_ = true;
}

void Metrics::Impl::UpdateScheme(unsigned long long now) {
    if (scheme_ready_ && now - scheme_tick_ < kSchemeEveryMs) return;
    diagnostics::ScopedTiming timing(L"Power scheme");
    scheme_ready_ = true;
    scheme_tick_ = now;
    OverlayFn fn = Overlay();
    GUID guid{};
    if (!fn || fn(&guid) != 0) {
        scheme_kind_ = PowerSchemeKind::Unknown;
        scheme_.clear();
        return;
    }
    scheme_kind_ = SchemeKind(guid);
    scheme_ = SchemeName(scheme_kind_);
}

void Metrics::Impl::UpdateActivePlan(unsigned long long now) {
    diagnostics::ScopedTiming timing(L"Active power plan");
    LoadPowerApi();
    GUID* heap = nullptr;
    if (!active_fn_ || active_fn_(nullptr, &heap) != ERROR_SUCCESS || !heap) {
        if (heap) LocalFree(heap);
        active_valid_ = false;
        active_name_.clear();
        have_active_guid_ = false;
        return;
    }
    const GUID guid = *heap;
    LocalFree(heap);
    const bool changed = !have_active_guid_ || !SameGuid(guid, active_guid_);
    active_guid_ = guid;
    have_active_guid_ = true;
    // The GUID is checked every full sample. The friendly name follows a rename
    // on the same GUID at the existing five-second cadence.
    if (!changed && active_valid_ && now - active_name_tick_ < kSchemeEveryMs) return;
    active_name_tick_ = now;
    std::wstring name;
    if (!ReadPlanName(friendly_fn_, guid, name)) {
        active_valid_ = false;
        active_name_.clear();
        return;
    }
    active_valid_ = true;
    active_name_ = std::move(name);
}

bool Metrics::Impl::EnsureEnergy(bool full, unsigned long long now) {
    if (energy_ok_) return true;
    if (now < energy_retry_at_) return false;
    energy_retry_at_ = now + kPdhRetryMs;
    if (energy_query_) PdhCloseQuery(energy_query_);
    energy_query_ = nullptr;
    energy_counter_ = power_counter_ = energy_time_counter_ = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &energy_query_) != ERROR_SUCCESS) {
        energy_query_ = nullptr;
        meter_status_.fill(MeterStatus::CounterUnavailable);
        return false;
    }
    // Preserve compact mode's single Power counter and its existing cadence.
    // Full mode prefers unscaled cumulative Energy and keeps Power as a fallback.
    if (full && PdhAddEnglishCounterW(energy_query_, L"\\Energy Meter(*)\\Energy", 0,
                                     &energy_counter_) != ERROR_SUCCESS)
        energy_counter_ = nullptr;
    if (full && energy_counter_ &&
        PdhAddEnglishCounterW(energy_query_, L"\\Energy Meter(*)\\Time", 0, &energy_time_counter_) != ERROR_SUCCESS)
        energy_time_counter_ = nullptr;
    if (PdhAddEnglishCounterW(energy_query_, L"\\Energy Meter(*)\\Power", 0,
                             &power_counter_) != ERROR_SUCCESS)
        power_counter_ = nullptr;
    if (!energy_counter_ && !power_counter_) {
        PdhCloseQuery(energy_query_);
        energy_query_ = nullptr;
        meter_status_.fill(MeterStatus::CounterUnavailable);
        return false;
    }
    energy_ok_ = true;
    return true;
}

bool Metrics::Impl::ReadEnergy(bool full) {
    const std::array<double, 4> cached{{pkg_w_, cpu_w_, gpu_w_, mem_w_}};
    const bool cached_source = pkg_from_energy_;
    const double cached_delta = pkg_delta_pwh_, cached_seconds = pkg_interval_s_;
    pkg_from_energy_ = false;
    pkg_delta_pwh_ = pkg_interval_s_ = NAN;
    pkg_power_counter_w_ = NAN;
    if (PdhCollectQueryData(energy_query_) != ERROR_SUCCESS) {
        energy_prev_ = {};
        meter_status_.fill(MeterStatus::ReadFailed);
        return false;
    }
    const std::int64_t qpc = QpcNow();
    std::array<EnergyPoint, 4> current{};
    std::array<int, 4> best{};
    std::array<double, 4> watts{{NAN, NAN, NAN, NAN}};
    std::array<bool, 4> energy_named{}, power_named{};
    DWORD count = 0;
    const bool energy_array_ok = full && RawArray(energy_counter_, pdh_buf_, count);
    if (energy_array_ok) {
        const auto* items = reinterpret_cast<const PDH_RAW_COUNTER_ITEM_W*>(pdh_buf_.data());
        count = std::min(count, static_cast<DWORD>(pdh_buf_.size() / sizeof(PDH_RAW_COUNTER_ITEM_W)));
        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];
            if (!item.szName) continue;
            for (std::size_t d = 0; d < current.size(); ++d) {
                const int score = EnergyScore(item.szName, d);
                if (score > 0) energy_named[d] = true;
                if (!MeterDataValid(item.RawValue.CStatus)) continue;
                if (score <= best[d]) continue;
                best[d] = score;
                current[d] = {item.szName, static_cast<uint64_t>(item.RawValue.FirstValue), qpc};
            }
        }
    }
    // Some providers update at about 1 Hz even when queried at 0.5 s. Time is
    // only a freshness marker; the power denominator always remains QPC seconds.
    // Retain the last complete interval instead of alternating false zero/double
    // readings, and align a new meter before publishing its first full interval.
    if (full && RawArray(energy_time_counter_, pdh_buf_, count)) {
        const auto* items = reinterpret_cast<const PDH_RAW_COUNTER_ITEM_W*>(pdh_buf_.data());
        count = std::min(count, static_cast<DWORD>(pdh_buf_.size() / sizeof(PDH_RAW_COUNTER_ITEM_W)));
        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];
            if (!item.szName || (item.RawValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                                item.RawValue.CStatus != PDH_CSTATUS_NEW_DATA)) continue;
            for (auto& cur : current) {
                if (!cur.qpc || cur.name != item.szName) continue;
                cur.meter_time = static_cast<uint64_t>(item.RawValue.FirstValue);
                cur.have_meter_time = true;
            }
        }
    }
    bool have_meter = false;
    std::array<bool, 4> use_power{};
    for (std::size_t d = 0; d < current.size(); ++d) {
        auto& cur = current[d];
        const auto& prev = energy_prev_[d];
        if (!cur.qpc) { use_power[d] = true; continue; }
        have_meter = true;
        const double seconds = QpcSeconds(cur.qpc, prev.qpc);
        // First sample, reset/rollover, changed identity: store a fresh baseline.
        // Do not publish a Power sample as if it were a complete Energy interval.
        if (!prev.qpc || cur.name != prev.name || cur.pwh < prev.pwh || !(seconds > 0)) continue;
        if (cur.have_meter_time) {
            if (!prev.have_meter_time || cur.meter_time < prev.meter_time) continue;
            if (cur.meter_time == prev.meter_time) {
                cur = prev; // Keep QPC and energy at the previous fresh boundary.
                // A stopped provider is unavailable, not an indefinitely valid
                // cached Energy interval. Keep the baseline for its recovery.
                if (seconds > kEnergyStaleSec) { use_power[d] = true; continue; }
                watts[d] = cached[d];
                if (d == 0 && cached_source) {
                    pkg_from_energy_ = true;
                    pkg_delta_pwh_ = cached_delta;
                    pkg_interval_s_ = cached_seconds;
                }
                continue;
            }
            cur.aligned = true;
            if (!prev.aligned) continue;
        }
        // The raw Energy Meter counter is cumulative picowatt-hours (pWh).
        // 1 pWh = 3.6e-9 joules; subtract integers BEFORE converting to double.
        // Microsoft EMI: learn.microsoft.com/windows-hardware/drivers/powermeter/energy-meter-interface
        const uint64_t delta = cur.pwh - prev.pwh;
        const double value = static_cast<double>(delta) * 3.6e-9 / seconds;
        if (!std::isfinite(value) || value > 1.0e6) { use_power[d] = true; continue; }
        watts[d] = value;
        if (d == 0) {
            pkg_from_energy_ = true;
            pkg_delta_pwh_ = static_cast<double>(delta);
            pkg_interval_s_ = seconds;
        }
    }
    energy_prev_ = std::move(current);
    // Full mode also exposes a same-collection Power reference for unit checks.
    // Compact mode keeps its existing single formatted Power-array read.
    std::array<int, 4> power_best{};
    const bool power_format_ok = FormatArray(power_counter_, pdh_buf_, count);
    if (power_format_ok) {
        const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(pdh_buf_.data());
        count = std::min(count, static_cast<DWORD>(pdh_buf_.size() / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W)));
        std::array<double, 4> power_watts{{NAN, NAN, NAN, NAN}};
        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];
            const double value = item.FmtValue.doubleValue;
            if (!item.szName) continue;
            for (std::size_t d = 0; d < watts.size(); ++d) {
                const int score = EnergyScore(item.szName, d);
                if (score > 0) power_named[d] = true;
                if (!MeterDataValid(item.FmtValue.CStatus) || !MeterPowerValid(value)) continue;
                if (score <= power_best[d]) continue;
                power_best[d] = score;
                power_watts[d] = value / 1000.0; // Power is mW.
                have_meter = true;
            }
        }
        if (full) pkg_power_counter_w_ = power_watts[0];
        for (std::size_t d = 0; d < watts.size(); ++d) {
            if (use_power[d]) watts[d] = power_watts[d];
        }
    }
    for (std::size_t d = 0; d < watts.size(); ++d) {
        meter_status_[d] = ResolveMeterStatus(
            {full && energy_counter_ != nullptr, energy_array_ok, energy_named[d]},
            {power_counter_ != nullptr, power_format_ok, power_named[d]}, watts[d],
            full && best[d] > 0 && !use_power[d]);
        if (meter_status_[d] == MeterStatus::Valid) have_meter = true;
    }
    pkg_w_ = watts[0]; cpu_w_ = watts[1]; gpu_w_ = watts[2]; mem_w_ = watts[3];
    return have_meter;
}

void Metrics::Impl::UpdateEnergy(bool full, unsigned long long now) {
    if (!full && have_energy_due_ && now < energy_due_) return;
    if (!energy_ok_ && now < energy_retry_at_) return;
    diagnostics::ScopedTiming timing(L"PDH Energy Meter");
    if (!full) {
        energy_due_ = NextDue(have_energy_due_, energy_due_, now, kEnergyEveryMs);
        have_energy_due_ = true;
    }
    if (!EnsureEnergy(full, now) || !ReadEnergy(full)) {
        pkg_w_ = cpu_w_ = gpu_w_ = mem_w_ = NAN;
        pkg_from_energy_ = false;
        pkg_delta_pwh_ = pkg_interval_s_ = NAN;
        pkg_power_counter_w_ = NAN;
    }
}

bool Metrics::Impl::EnsureGpu(unsigned long long now) {
    if (gpu_ok_) return true;
    if (now < gpu_retry_at_) return false;
    gpu_retry_at_ = now + kPdhRetryMs;
    if (gpu_query_) {
        PdhCloseQuery(gpu_query_);
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
    }
    if (PdhOpenQueryW(nullptr, 0, &gpu_query_) != ERROR_SUCCESS) {
        gpu_query_ = nullptr;
        return false;
    }
    if (PdhAddEnglishCounterW(gpu_query_, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &gpu_counter_) != ERROR_SUCCESS) {
        PdhCloseQuery(gpu_query_);
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
        return false;
    }
    gpu_ok_ = true;
    gpu_warmed_ = false;
    return true;
}

bool Metrics::Impl::ReadGpu(double& out) {
    DWORD count = 0;
    if (!QueryArray(gpu_query_, gpu_counter_, pdh_buf_, count, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100)) return false;
    const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(pdh_buf_.data());
    count = std::min(count, static_cast<DWORD>(pdh_buf_.size() / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W)));
    gpu_buckets_.clear();
    std::wstring adapter, engine;
    for (DWORD i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        const double value = item.FmtValue.doubleValue;
        if (!std::isfinite(value) || value < 0 || !GpuIdentity(item.szName, adapter, engine)) continue;
        auto bucket = std::find_if(gpu_buckets_.begin(), gpu_buckets_.end(), [&](const GpuBucket& b) {
            return b.adapter == adapter && b.engine == engine;
        });
        if (bucket == gpu_buckets_.end()) gpu_buckets_.push_back({adapter, engine, value});
        else bucket->usage += value;
    }
    if (gpu_buckets_.empty()) return false;
    double busiest = 0;
    for (const auto& bucket : gpu_buckets_) busiest = std::max(busiest, bucket.usage);
    // Max across engine types AND adapters: independent GPUs/types cannot add
    // together. Instances of the same type on the same LUID have already summed.
    out = std::clamp(busiest, 0.0, 100.0);
    return true;
}

void Metrics::Impl::UpdateGpu(bool full, unsigned long long now) {
    if (!full && gpu_ok_ && gpu_warmed_ && now < gpu_due_) return;
    if (!gpu_ok_ && now < gpu_retry_at_) return;
    diagnostics::ScopedTiming timing(L"PDH GPU Engine");
    if (!EnsureGpu(now)) { gpu_percent_ = NAN; return; }
    if (!gpu_warmed_) {
        // A rate counter requires two successful collections. A failed warmup
        // must not arm the next formatted read.
        gpu_warmed_ = PdhCollectQueryData(gpu_query_) == ERROR_SUCCESS;
        gpu_due_ = now + kGpuEveryMs;
        gpu_percent_ = NAN;
        return;
    }
    if (!full) gpu_due_ = NextDue(true, gpu_due_, now, kGpuEveryMs);
    double value = NAN;
    if (!ReadGpu(value)) {
        gpu_percent_ = NAN;
        gpu_warmed_ = false;
    } else gpu_percent_ = value;
}

void Metrics::Impl::CloseQueries() {
    ReleaseFps(L"shutdown");
    ReleaseEnergy();
    ReleaseGpu();
    ReleaseDisk();
    ReleaseCpuQuery();
    ReleaseGpuMem();
    ReleaseGpuTemp();
    dgpu_.Reset();
    ReleaseWifi();
    ReleaseProcs();
    ReleaseSysDisk();
    ReleaseBatteryHealth();
}

Wants Metrics::Impl::CurrentWants() const {
    return UnpackWants(want_bits_.load(std::memory_order_acquire));
}

void Metrics::Impl::ClearTurnedOff(Snapshot& s, const Wants& w) {
    if (!w.chip_power) {
        s.pkg_w = NAN;
        s.cpu_w = NAN;
        s.gpu_w = NAN;
        s.mem_w = NAN;
        s.pkg_power_from_energy = false;
        s.pkg_energy_delta_pwh = s.pkg_energy_interval_s = NAN;
        s.pkg_power_counter_w = NAN;
        s.pkg_meter = s.cpu_meter = s.gpu_meter = s.mem_meter = MeterStatus::Off;
    }
    if (!w.gpu_usage) s.gpu_percent = NAN;
    if (!w.processes) {
        s.top_procs.clear();
        s.self_cpu_percent = NAN;
    }
    if (!w.processes && !w.process_count) s.process_count = -1;
    if (!w.power_scheme) {
        s.power_scheme.clear();
        s.power_scheme_kind = PowerSchemeKind::Unknown;
        s.active_plan_valid = false;
        s.active_plan_name.clear();
        s.active_plan_balanced = false;
    }
    if (!w.memory_detail) {
        s.mem_used_gb = NAN;
        s.mem_total_gb = NAN;
    }
    if (!w.disk_io) {
        s.disk_read_bps = NAN;
        s.disk_write_bps = NAN;
    }
    if (!w.sys_disk) {
        s.sys_disk_used_percent = NAN;
        s.sys_disk_free_gb = NAN;
    }
    if (!w.cpu_freq) s.cpu_mhz = NAN;
    if (!w.uptime) s.uptime_s = NAN;
    if (!w.gpu_temp) s.gpu_temp_c = NAN;
    if (!w.dgpu_power) {
        s.dgpu_status = DgpuStatus::Off;
        s.dgpu_w = NAN;
        dgpu_.Reset();
    }
    if (!w.gpu_memory) {
        s.gpu_shared_mem_mb = NAN;
        s.gpu_dedicated_mem_mb = NAN;
    }
    if (!w.wifi) {
        s.wifi_ssid.clear();
        s.wifi_signal = -1;
    }
    if (!w.battery_health) {
        s.battery_health_percent = NAN;
        s.battery_cycles = -1;
    }
    if (!w.fps) {
        s.fps = NAN;
        s.fps_status = 0;
        s.fps_process.clear();
        s.fps_top.clear();
    }
}

void Metrics::Impl::ReleaseEnergy() {
    meter_status_.fill(MeterStatus::Off);
    if (!energy_query_ && !energy_ok_ && !have_energy_due_ && !energy_retry_at_ &&
        std::isnan(pkg_w_) && std::isnan(cpu_w_) && std::isnan(gpu_w_) && std::isnan(mem_w_)) return;
    if (energy_query_) PdhCloseQuery(energy_query_);
    energy_query_ = nullptr;
    energy_counter_ = power_counter_ = energy_time_counter_ = nullptr;
    energy_ok_ = false;
    have_energy_due_ = false;
    energy_due_ = energy_retry_at_ = 0;
    energy_prev_ = {};
    pkg_w_ = cpu_w_ = gpu_w_ = mem_w_ = NAN;
    pkg_from_energy_ = false;
    pkg_delta_pwh_ = pkg_interval_s_ = NAN;
    pkg_power_counter_w_ = NAN;
}

void Metrics::Impl::ReleaseGpu() {
    if (!gpu_query_ && !gpu_ok_ && !gpu_warmed_ && !gpu_retry_at_ &&
        gpu_buckets_.empty() && std::isnan(gpu_percent_)) return;
    if (gpu_query_) PdhCloseQuery(gpu_query_);
    gpu_query_ = nullptr;
    gpu_counter_ = nullptr;
    gpu_ok_ = false;
    gpu_warmed_ = false;
    gpu_due_ = 0;
    gpu_retry_at_ = 0;
    gpu_percent_ = NAN;
    std::vector<GpuBucket>().swap(gpu_buckets_);
}

void Metrics::Impl::ReleaseProcs() {
    if (!procs_buffers_ && top_cache_.empty() && process_count_ < 0 && std::isnan(self_cache_)) return;
    procs_buffers_ = false;
    procs_.Release();
    top_cache_.clear();
    top_cache_.shrink_to_fit();
    self_cache_ = NAN;
    process_count_ = -1;
    procs_due_ = 0;
}

void Metrics::Impl::ReleaseDisk() {
    if (!disk_query_ && !disk_ok_ && !disk_warmed_ && !have_disk_due_ && std::isnan(disk_read_) &&
        std::isnan(disk_write_)) {
        return;
    }
    if (disk_query_) PdhCloseQuery(disk_query_);
    disk_query_ = nullptr;
    disk_read_counter_ = nullptr;
    disk_write_counter_ = nullptr;
    disk_ok_ = false;
    disk_warmed_ = false;
    have_disk_due_ = false;
    disk_due_ = 0;
    disk_retry_at_ = 0;
    disk_read_ = NAN;
    disk_write_ = NAN;
}

void Metrics::Impl::ReleaseCpuFreq() {
    if (cpu_freq_perf_) PdhRemoveCounter(cpu_freq_perf_);
    if (cpu_freq_base_) PdhRemoveCounter(cpu_freq_base_);
    cpu_freq_perf_ = nullptr;
    cpu_freq_base_ = nullptr;
    cpu_freq_ok_ = false;
    cpu_freq_warmed_ = false;
    cpu_freq_retry_at_ = 0;
    cpu_mhz_ = NAN;
}

void Metrics::Impl::ReleaseCpuQuery() {
    ReleaseCpuFreq();
    if (cpu_query_) PdhCloseQuery(cpu_query_);
    cpu_query_ = nullptr;
    cpu_utility_ = nullptr;
    cpu_utility_warmed_ = false;
    have_cpu_query_tick_ = false;
    cpu_query_tick_ = 0;
    cpu_query_retry_at_ = cpu_utility_retry_at_ = 0;
}

void Metrics::Impl::ReleaseGpuMem() {
    if (!gpu_mem_query_ && !gpu_mem_ok_ && !have_gpu_mem_due_ && std::isnan(gpu_shared_mb_) &&
        std::isnan(gpu_dedicated_mb_)) {
        return;
    }
    if (gpu_mem_query_) PdhCloseQuery(gpu_mem_query_);
    gpu_mem_query_ = nullptr;
    gpu_mem_shared_ = nullptr;
    gpu_mem_dedicated_ = nullptr;
    gpu_mem_ok_ = false;
    have_gpu_mem_due_ = false;
    gpu_mem_due_ = 0;
    gpu_mem_retry_at_ = 0;
    gpu_shared_mb_ = NAN;
    gpu_dedicated_mb_ = NAN;
}

void Metrics::Impl::ReleaseGpuTemp() {
    if (!gpu_temp_api_.module && !gpu_temp_ready_ && !have_gpu_temp_due_ &&
        gpu_temp_adapters_.empty() && gpu_temp_retry_at_ == 0) return;
    PulseKmtCloseAdapters(gpu_temp_api_, gpu_temp_adapters_);
    std::vector<PulseKmtAdapterInfo>().swap(gpu_temp_adapters_);
    gpu_temp_api_.Reset();
    gpu_temp_ready_ = have_gpu_temp_due_ = gpu_temp_seen_ = false;
    gpu_temp_due_ = gpu_temp_retry_at_ = gpu_temp_enum_at_ = 0;
    gpu_temp_c_ = NAN;
}

void Metrics::Impl::ReleaseWifi() {
    if (!wifi_mod_ && !wifi_client_ && !have_wifi_due_ && wifi_ssid_.empty() && wifi_signal_ < 0) return;
    if (wifi_client_ && wifi_close_) wifi_close_(wifi_client_, nullptr);
    wifi_client_ = nullptr;
    wifi_open_ = nullptr;
    wifi_close_ = nullptr;
    wifi_enum_ = nullptr;
    wifi_query_ = nullptr;
    wifi_free_ = nullptr;
    if (wifi_mod_) {
        FreeLibrary(wifi_mod_);
        wifi_mod_ = nullptr;
    }
    wifi_tried_ = false;
    have_wifi_due_ = false;
    wifi_due_ = 0;
    wifi_retry_at_ = 0;
    wifi_ssid_.clear();
    wifi_signal_ = -1;
}

void Metrics::Impl::ReleaseSysDisk() {
    if (!have_sys_due_ && std::isnan(sys_used_pct_) && std::isnan(sys_free_gb_)) return;
    have_sys_due_ = false;
    sys_due_ = 0;
    sys_used_pct_ = NAN;
    sys_free_gb_ = NAN;
}

void Metrics::Impl::ReleaseBatteryHealth() {
    if (!have_battery_due_ && std::isnan(battery_health_) && battery_cycles_ < 0) return;
    have_battery_due_ = false;
    battery_due_ = 0;
    battery_health_ = NAN;
    battery_cycles_ = -1;
}

bool Metrics::Impl::EnsureDisk(unsigned long long now) {
    if (disk_ok_) return true;
    if (now < disk_retry_at_) return false;
    disk_retry_at_ = now + kPdhRetryMs;
    const CounterBind binds[] = {
        {L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &disk_read_counter_},
        {L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &disk_write_counter_},
    };
    disk_warmed_ = false;
    disk_ok_ = OpenEnglishQuery(disk_query_, binds, 2);
    return disk_ok_;
}

bool Metrics::Impl::EnsureCpuQuery(unsigned long long now) {
    if (cpu_query_) return true;
    if (now < cpu_query_retry_at_) return false;
    cpu_query_retry_at_ = now + kPdhRetryMs;
    if (PdhOpenQueryW(nullptr, 0, &cpu_query_) != ERROR_SUCCESS) {
        cpu_query_ = nullptr;
        return false;
    }
    return true;
}

bool Metrics::Impl::EnsureCpuUtility(unsigned long long now) {
    if (cpu_utility_) return true;
    if (now < cpu_utility_retry_at_) return false;
    cpu_utility_retry_at_ = now + kPdhRetryMs;
    if (!EnsureCpuQuery(now)) return false;
    cpu_utility_warmed_ = false;
    if (PdhAddEnglishCounterW(cpu_query_, L"\\Processor Information(_Total)\\% Processor Utility", 0,
                              &cpu_utility_) != ERROR_SUCCESS) {
        cpu_utility_ = nullptr;
        return false;
    }
    return true;
}

bool Metrics::Impl::EnsureCpuFreq(unsigned long long now) {
    if (cpu_freq_ok_) return true;
    if (now < cpu_freq_retry_at_) return false;
    cpu_freq_retry_at_ = now + kPdhRetryMs;
    if (!EnsureCpuQuery(now)) return false;
    cpu_freq_warmed_ = false;
    // Frequency failures must not close the shared query or drop Utility.
    if (PdhAddEnglishCounterW(cpu_query_, L"\\Processor Information(_Total)\\% Processor Performance", 0,
                              &cpu_freq_perf_) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(cpu_query_, L"\\Processor Information(_Total)\\Processor Frequency", 0,
                              &cpu_freq_base_) != ERROR_SUCCESS) {
        if (cpu_freq_perf_) PdhRemoveCounter(cpu_freq_perf_);
        if (cpu_freq_base_) PdhRemoveCounter(cpu_freq_base_);
        cpu_freq_perf_ = cpu_freq_base_ = nullptr;
        return false;
    }
    cpu_freq_ok_ = true;
    return cpu_freq_ok_;
}

bool Metrics::Impl::EnsureGpuMem(unsigned long long now) {
    if (gpu_mem_ok_) return true;
    if (now < gpu_mem_retry_at_) return false;
    gpu_mem_retry_at_ = now + kPdhRetryMs;
    const CounterBind binds[] = {
        {L"\\GPU Adapter Memory(*)\\Shared Usage", &gpu_mem_shared_},
        {L"\\GPU Adapter Memory(*)\\Dedicated Usage", &gpu_mem_dedicated_},
    };
    gpu_mem_ok_ = OpenEnglishQuery(gpu_mem_query_, binds, 2);
    return gpu_mem_ok_;
}

bool Metrics::Impl::EnsureGpuTemp(unsigned long long now) {
    if (gpu_temp_ready_) return true;
    if (now < gpu_temp_retry_at_) return false;
    diagnostics::ScopedTiming timing(L"D3DKMT GPU adapters");
    gpu_temp_retry_at_ = now + kGpuTempRetryMs;
    if (!gpu_temp_api_.Load()) return false;
    PulseKmtEnumAdapters2 request{};
    // The first call returns the supported capacity without opening handles.
    if (gpu_temp_api_.enumerate(&request) < 0 || request.NumAdapters > 4096) return false;
    gpu_temp_adapters_.assign(request.NumAdapters, PulseKmtAdapterInfo{});
    if (request.NumAdapters != 0) {
        request.pAdapters = gpu_temp_adapters_.data();
        const LONG status = gpu_temp_api_.enumerate(&request);
        // NumAdapters is undefined on failure: inspect only our zero-initialized
        // allocation when closing any partially returned handles.
        if (status < 0 || request.NumAdapters > gpu_temp_adapters_.size()) {
            PulseKmtCloseAdapters(gpu_temp_api_, gpu_temp_adapters_);
            return false;
        }
        for (auto& adapter : gpu_temp_adapters_) {
            if (!adapter.hAdapter) continue;
            PulseKmtAdapterType type{};
            const PulseKmtQueryAdapterInfo query{adapter.hAdapter, kPulseKmtAdapterType,
                &type, static_cast<UINT>(sizeof(type))};
            if (gpu_temp_api_.query(&query) < 0) {
                PulseKmtCloseAdapters(gpu_temp_api_, gpu_temp_adapters_);
                return false;
            }
            if ((type.Value & kPulseKmtSoftwareDevice) || !(type.Value & kPulseKmtRenderSupported)) {
                const PulseKmtCloseAdapter close{adapter.hAdapter};
                gpu_temp_api_.close(&close);
                adapter.hAdapter = 0;
            }
        }
        gpu_temp_adapters_.erase(std::remove_if(gpu_temp_adapters_.begin(), gpu_temp_adapters_.end(),
            [](const PulseKmtAdapterInfo& adapter) { return adapter.hAdapter == 0; }), gpu_temp_adapters_.end());
    }
    gpu_temp_ready_ = true;
    gpu_temp_enum_at_ = now;
    return true;
}

bool Metrics::Impl::EnsureWifi(unsigned long long now) {
    if (wifi_client_) return true;
    if (wifi_tried_ && now < wifi_retry_at_) return false;
    wifi_tried_ = true;
    wifi_retry_at_ = now + kPdhRetryMs;
    if (!wifi_mod_) {
        wifi_mod_ = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!wifi_mod_) return false;
        wifi_open_ = reinterpret_cast<WlanOpenHandleFn>(GetProcAddress(wifi_mod_, "WlanOpenHandle"));
        wifi_close_ = reinterpret_cast<WlanCloseHandleFn>(GetProcAddress(wifi_mod_, "WlanCloseHandle"));
        wifi_enum_ = reinterpret_cast<WlanEnumInterfacesFn>(GetProcAddress(wifi_mod_, "WlanEnumInterfaces"));
        wifi_query_ = reinterpret_cast<WlanQueryInterfaceFn>(GetProcAddress(wifi_mod_, "WlanQueryInterface"));
        wifi_free_ = reinterpret_cast<WlanFreeMemoryFn>(GetProcAddress(wifi_mod_, "WlanFreeMemory"));
        if (!wifi_open_ || !wifi_close_ || !wifi_enum_ || !wifi_query_ || !wifi_free_) {
            wifi_open_ = nullptr;
            wifi_close_ = nullptr;
            wifi_enum_ = nullptr;
            wifi_query_ = nullptr;
            wifi_free_ = nullptr;
            FreeLibrary(wifi_mod_);
            wifi_mod_ = nullptr;
            return false;
        }
    }
    DWORD version = 0;
    HANDLE handle = nullptr;
    if (wifi_open_(2, nullptr, &version, &handle) != ERROR_SUCCESS || !handle) return false;
    wifi_client_ = handle;
    return true;
}

PdhRead Metrics::Impl::ReadDisk(double& read_bps, double& write_bps) {
    if (!disk_query_ || PdhCollectQueryData(disk_query_) != ERROR_SUCCESS) return PdhRead::Fail;
    if (!disk_warmed_) {
        disk_warmed_ = true;
        return PdhRead::Warmup;
    }
    double read_v = NAN;
    double write_v = NAN;
    const bool read_ok = FormatDouble(disk_read_counter_, read_v) && read_v >= 0.0;
    const bool write_ok = FormatDouble(disk_write_counter_, write_v) && write_v >= 0.0;
    if (!read_ok && !write_ok) return PdhRead::Fail;
    read_bps = read_ok ? read_v : NAN;
    write_bps = write_ok ? write_v : NAN;
    return PdhRead::Ok;
}

PdhRead Metrics::Impl::ReadCpuFreq(double& mhz) {
    if (!cpu_query_ || !cpu_freq_perf_ || !cpu_freq_base_) return PdhRead::Fail;
    if (!cpu_freq_warmed_) {
        cpu_freq_warmed_ = true;
        return PdhRead::Warmup;
    }
    // _Total is an aggregate across logical processors, not one core's clock.
    double perf = NAN;
    double base = NAN;
    if (!FormatDouble(cpu_freq_perf_, perf) || !FormatDouble(cpu_freq_base_, base)) return PdhRead::Fail;
    if (!(perf >= 0.0) || !(base > 0.0)) return PdhRead::Fail;
    const double value = perf * base / 100.0;
    if (!std::isfinite(value) || value <= 0.0 || value > 20000.0) return PdhRead::Fail;
    mhz = value;
    return PdhRead::Ok;
}

bool Metrics::Impl::CopyInstances(PDH_HCOUNTER counter, std::vector<std::pair<std::wstring, double>>& out) {
    DWORD count = 0;
    if (!FormatArray(counter, pdh_buf_, count)) return false;
    const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(pdh_buf_.data());
    const DWORD max_items = static_cast<DWORD>(pdh_buf_.size() / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    if (count > max_items) count = max_items;
    out.clear();
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        const DWORD status = items[i].FmtValue.CStatus;
        if (status != PDH_CSTATUS_VALID_DATA && status != PDH_CSTATUS_NEW_DATA) continue;
        const double value = items[i].FmtValue.doubleValue;
        if (!std::isfinite(value) || value < 0.0 || value > 1.0e14) continue;
        const wchar_t* name = items[i].szName;
        if (!name || !name[0] || std::wcscmp(name, L"_Total") == 0) continue;
        out.emplace_back(name, value);
    }
    return true;
}

bool Metrics::Impl::ReadGpuMemory(double& shared_mb, double& dedicated_mb) {
    // PDH may expose several physical-node instances for the same adapter LUID.
    // Merge them before counting adapters. Dedicated Usage must be positive,
    // with at least two distinct adapter LUIDs, to report discrete memory.
    if (!gpu_mem_query_ || PdhCollectQueryData(gpu_mem_query_) != ERROR_SUCCESS) return false;
    std::vector<std::pair<std::wstring, double>> shared_items;
    std::vector<std::pair<std::wstring, double>> dedicated_items;
    const bool got_shared = CopyInstances(gpu_mem_shared_, shared_items);
    const bool got_dedicated = CopyInstances(gpu_mem_dedicated_, dedicated_items);
    if (!got_shared && !got_dedicated) return false;
    struct Row {
        std::uint64_t luid = 0;
        double shared = -1.0;
        double dedicated = -1.0;
    };
    std::vector<Row> rows;
    auto adapter = [&rows](const std::wstring& name) -> Row* {
        const auto start = name.find(L"luid_");
        unsigned long high = 0, low = 0;
        if (start == std::wstring::npos ||
            std::swscanf(name.c_str() + start, L"luid_%lx_%lx", &high, &low) != 2) return nullptr;
        const std::uint64_t luid = (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
        for (auto& row : rows) {
            if (row.luid == luid) return &row;
        }
        Row row;
        row.luid = luid;
        rows.push_back(std::move(row));
        return &rows.back();
    };
    if (got_shared) {
        for (const auto& item : shared_items) {
            if (auto* row = adapter(item.first)) row->shared = std::max(0.0, row->shared) + item.second;
        }
    }
    if (got_dedicated) {
        for (const auto& item : dedicated_items) {
            if (auto* row = adapter(item.first)) row->dedicated = std::max(0.0, row->dedicated) + item.second;
        }
    }
    if (rows.empty()) return false;
    double shared_sum = 0.0;
    bool any_shared = false;
    double dedicated_max = 0.0;
    for (const Row& row : rows) {
        if (row.shared >= 0.0) {
            any_shared = true;
            shared_sum += row.shared;
        }
        if (row.dedicated >= 0.0) {
            if (row.dedicated > dedicated_max) dedicated_max = row.dedicated;
        }
    }
    shared_mb = any_shared ? shared_sum / kMiB : NAN;
    dedicated_mb = (rows.size() >= 2 && dedicated_max > 0.0) ? dedicated_max / kMiB : NAN;
    return true;
}

bool Metrics::Impl::ReadWifi(std::wstring& ssid, int& signal) {
    if (!wifi_client_ || !wifi_enum_ || !wifi_query_ || !wifi_free_) return false;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (wifi_enum_(wifi_client_, nullptr, &list) != ERROR_SUCCESS || !list) return false;
    bool connected = false;
    bool query_failed = false;
    std::wstring found;
    int quality = -1;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& info = list->InterfaceInfo[i];
        if (info.isState != wlan_interface_state_connected) continue;
        PVOID data = nullptr;
        DWORD size = 0;
        WLAN_OPCODE_VALUE_TYPE kind{};
        const DWORD rc = wifi_query_(wifi_client_, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr,
                                      &size, &data, &kind);
        if (rc != ERROR_SUCCESS || !data) {
            if (data) wifi_free_(data);
            query_failed = true;
            continue;
        }
        const auto* attr = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data);
        if (attr->isState == wlan_interface_state_connected) {
            const DOT11_SSID& dot = attr->wlanAssociationAttributes.dot11Ssid;
            found = SsidToWide(dot.ucSSID, dot.uSSIDLength);
            const unsigned long q = attr->wlanAssociationAttributes.wlanSignalQuality;
            quality = q > 100ul ? 100 : static_cast<int>(q);
            connected = true;
        }
        wifi_free_(data);
        if (connected) break;
    }
    wifi_free_(list);
    if (!connected) {
        if (query_failed) return false;
        ssid.clear();
        signal = -1;
        return true;
    }
    ssid = std::move(found);
    signal = quality;
    return true;
}

void Metrics::Impl::UpdateDisk(unsigned long long now, Snapshot& s) {
    if (have_disk_due_ && disk_warmed_ && now < disk_due_) {
        s.disk_read_bps = disk_read_;
        s.disk_write_bps = disk_write_;
        return;
    }
    if (!disk_ok_ && now < disk_retry_at_) {
        s.disk_read_bps = disk_read_;
        s.disk_write_bps = disk_write_;
        return;
    }
    diagnostics::ScopedTiming timing(L"PDH Disk IO");
    disk_due_ = NextDue(have_disk_due_, disk_due_, now, kDiskEveryMs);
    have_disk_due_ = true;
    if (!EnsureDisk(now)) {
        disk_read_ = NAN;
        disk_write_ = NAN;
    } else {
        double read_bps = NAN;
        double write_bps = NAN;
        const PdhRead result = ReadDisk(read_bps, write_bps);
        if (result == PdhRead::Ok) {
            disk_read_ = read_bps;
            disk_write_ = write_bps;
        } else if (result == PdhRead::Fail) {
            disk_read_ = NAN;
            disk_write_ = NAN;
        }
    }
    s.disk_read_bps = disk_read_;
    s.disk_write_bps = disk_write_;
}

void Metrics::Impl::UpdateSysDisk(unsigned long long now, Snapshot& s) {
    if (have_sys_due_ && now < sys_due_) {
        s.sys_disk_used_percent = sys_used_pct_;
        s.sys_disk_free_gb = sys_free_gb_;
        return;
    }
    diagnostics::ScopedTiming timing(L"System disk");
    sys_due_ = NextDue(have_sys_due_, sys_due_, now, kSysDiskEveryMs);
    have_sys_due_ = true;
    if (!sys_root_ready_) {
        wchar_t dir[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH || dir[1] != L':') {
            s.sys_disk_used_percent = sys_used_pct_;
            s.sys_disk_free_gb = sys_free_gb_;
            return;
        }
        sys_root_[0] = dir[0];
        sys_root_[1] = L':';
        sys_root_[2] = L'\\';
        sys_root_[3] = L'\0';
        sys_root_ready_ = true;
    }
    ULARGE_INTEGER avail{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER free_bytes{};
    if (!GetDiskFreeSpaceExW(sys_root_, &avail, &total, &free_bytes) || total.QuadPart == 0) {
        s.sys_disk_used_percent = sys_used_pct_;
        s.sys_disk_free_gb = sys_free_gb_;
        return;
    }
    const double total_b = static_cast<double>(total.QuadPart);
    const double free_b = static_cast<double>(free_bytes.QuadPart);
    sys_free_gb_ = free_b / kGiB;
    const double used = total_b > free_b ? total_b - free_b : 0.0;
    sys_used_pct_ = used / total_b * 100.0;
    s.sys_disk_used_percent = sys_used_pct_;
    s.sys_disk_free_gb = sys_free_gb_;
}

void Metrics::Impl::UpdateCpuFreq(Snapshot& s) {
    diagnostics::ScopedTiming timing(L"PDH CPU frequency");
    if (!cpu_freq_ok_) {
        cpu_mhz_ = NAN;
    } else {
        double mhz = NAN;
        const PdhRead result = ReadCpuFreq(mhz);
        if (result == PdhRead::Ok) cpu_mhz_ = mhz;
        else if (result == PdhRead::Fail) cpu_mhz_ = NAN;
    }
    s.cpu_mhz = cpu_mhz_;
}

void Metrics::Impl::UpdateUptime(Snapshot& s) {
    diagnostics::ScopedTiming timing(L"Uptime");
    s.uptime_s = static_cast<double>(GetTickCount64()) / 1000.0;
}

void Metrics::Impl::UpdateGpuMemory(unsigned long long now, Snapshot& s) {
    if (have_gpu_mem_due_ && now < gpu_mem_due_) {
        s.gpu_shared_mem_mb = gpu_shared_mb_;
        s.gpu_dedicated_mem_mb = gpu_dedicated_mb_;
        return;
    }
    if (!gpu_mem_ok_ && now < gpu_mem_retry_at_) {
        s.gpu_shared_mem_mb = gpu_shared_mb_;
        s.gpu_dedicated_mem_mb = gpu_dedicated_mb_;
        return;
    }
    diagnostics::ScopedTiming timing(L"PDH GPU memory");
    gpu_mem_due_ = NextDue(have_gpu_mem_due_, gpu_mem_due_, now, kGpuMemEveryMs);
    have_gpu_mem_due_ = true;
    double shared = NAN;
    double dedicated = NAN;
    if (!EnsureGpuMem(now) || !ReadGpuMemory(shared, dedicated)) {
        gpu_shared_mb_ = NAN;
        gpu_dedicated_mb_ = NAN;
    } else {
        gpu_shared_mb_ = shared;
        gpu_dedicated_mb_ = dedicated;
    }
    s.gpu_shared_mem_mb = gpu_shared_mb_;
    s.gpu_dedicated_mem_mb = gpu_dedicated_mb_;
}

void Metrics::Impl::UpdateGpuTemp(unsigned long long now, Snapshot& s) {
    if ((have_gpu_temp_due_ && now < gpu_temp_due_) ||
        (!gpu_temp_ready_ && now < gpu_temp_retry_at_)) {
        s.gpu_temp_c = gpu_temp_c_;
        return;
    }
    gpu_temp_due_ = NextDue(have_gpu_temp_due_, gpu_temp_due_, now, kGpuTempEveryMs);
    have_gpu_temp_due_ = true;
    // An unsupported/sleeping GPU may become available later. Keep handles for
    // two-second samples; only rediscover once a minute until any value is valid.
    if (gpu_temp_ready_ && !gpu_temp_seen_ && now - gpu_temp_enum_at_ >= kGpuTempRediscoverMs) {
        PulseKmtCloseAdapters(gpu_temp_api_, gpu_temp_adapters_);
        gpu_temp_ready_ = false;
    }
    gpu_temp_c_ = NAN;
    if (EnsureGpuTemp(now)) {
        diagnostics::ScopedTiming timing(L"D3DKMT GPU temperature");
        ULONG hottest = 0;
        bool failed = false;
        for (const auto& adapter : gpu_temp_adapters_) {
            PulseKmtAdapterPerfData data{};
            data.PhysicalAdapterIndex = 0;
            const PulseKmtQueryAdapterInfo query{adapter.hAdapter, kPulseKmtAdapterPerfData,
                &data, static_cast<UINT>(sizeof(data))};
            if (gpu_temp_api_.query(&query) < 0) {
                failed = true;
                break;
            }
            if (data.Temperature > 0 && data.Temperature < 1500)
                hottest = std::max(hottest, data.Temperature);
        }
        if (failed) {
            // Driver resets/hotplug invalidate the whole enumeration. Discard
            // this round, even if another adapter already returned a value.
            PulseKmtCloseAdapters(gpu_temp_api_, gpu_temp_adapters_);
            gpu_temp_ready_ = false;
            gpu_temp_retry_at_ = GetTickCount64() + kGpuTempRetryMs;
        } else if (hottest != 0) {
            gpu_temp_c_ = hottest / 10.0;
            gpu_temp_seen_ = true;
        }
    }
    s.gpu_temp_c = gpu_temp_c_;
}

void Metrics::Impl::UpdateWifi(unsigned long long now, Snapshot& s) {
    if (have_wifi_due_ && now < wifi_due_) {
        s.wifi_ssid = wifi_ssid_;
        s.wifi_signal = wifi_signal_;
        return;
    }
    if (!wifi_client_ && wifi_tried_ && now < wifi_retry_at_) {
        s.wifi_ssid = wifi_ssid_;
        s.wifi_signal = wifi_signal_;
        return;
    }
    diagnostics::ScopedTiming timing(L"Wi-Fi");
    wifi_due_ = NextDue(have_wifi_due_, wifi_due_, now, kWifiEveryMs);
    have_wifi_due_ = true;
    if (!EnsureWifi(now)) {
        wifi_ssid_.clear();
        wifi_signal_ = -1;
    } else {
        std::wstring ssid;
        int signal = -1;
        if (ReadWifi(ssid, signal)) {
            wifi_ssid_ = std::move(ssid);
            wifi_signal_ = signal;
        }
    }
    s.wifi_ssid = wifi_ssid_;
    s.wifi_signal = wifi_signal_;
}

void Metrics::Impl::UpdateBatteryHealth(unsigned long long now, Snapshot& s) {
    if (have_battery_due_ && now < battery_due_) {
        s.battery_health_percent = battery_health_;
        s.battery_cycles = battery_cycles_;
        return;
    }
    diagnostics::ScopedTiming timing(L"Battery health");
    have_battery_due_ = true;
    double health = NAN;
    int cycles = -1;
    if (!ReadBatteryHealth(health, cycles)) {
        battery_health_ = NAN;
        battery_cycles_ = -1;
        battery_due_ = now + kPdhRetryMs;
    } else {
        battery_health_ = health;
        battery_cycles_ = cycles;
        battery_due_ = now + kBatteryHealthEveryMs;
    }
    s.battery_health_percent = battery_health_;
    s.battery_cycles = battery_cycles_;
}

void Metrics::Impl::ReleaseFps(const wchar_t* reason) {
    if (!fps_attempted_ && !fps_ok_ && fps_pid_ == 0 && fps_name_.empty() && fps_tree_.idle() &&
        fps_family_.empty()) {
        return;
    }
    fps_mon_.Stop();
    if (fps_attempted_ || fps_logged_status_ != 0) {
        diagnostics::WriteLog(L"FPS", L"status " + std::to_wstring(fps_logged_status_) +
            L"->0 ready=0 reason=" + reason + L" error=0");
    }
    fps_logged_status_ = 0;
    fps_logged_ready_ = false;
    fps_attempted_ = false;
    fps_ok_ = false;
    fps_retry_at_ = 0;
    fps_pid_ = 0;
    fps_name_.clear();
    fps_tree_.Clear();
    std::vector<DWORD>().swap(fps_family_);
}

void Metrics::Impl::UpdateFps(Snapshot& s, unsigned long long now) {
    const bool tried = !fps_attempted_ || (!fps_ok_ && now >= fps_retry_at_);
    if (tried) {
        fps_attempted_ = true;
        fps_ok_ = fps_mon_.Start();
    }
    // StartTrace can block. Steady state is the foreground query, a cached
    // parent walk, and one counter snapshot. Toolhelp runs only while the
    // session is up, and only every 3s or when the foreground process changes.
    diagnostics::ScopedTiming timing(L"FPS");
    const DWORD root = TrackedForegroundPid(fps_pid_);
    const bool root_changed = root != fps_pid_;
    if (root_changed) {
        fps_pid_ = root;
        fps_name_.clear();
    }
    fps_family_.clear();
    if (fps_mon_.Active()) {
        if (fps_tree_.Due(now, root_changed)) fps_tree_.Refresh(now);
        if (root != 0) fps_tree_.Collect(root, fps_family_);
    }
    if (root == 0) fps_name_.clear();
    else if (fps_name_.empty()) {
        if (!fps_tree_.Lookup(root, fps_name_) && !procs_.LookupName(root, fps_name_))
            fps_name_ = QueryImageBaseName(root);
    }
    double fps = NAN;
    int status = 0;
    DWORD source = 0;
    FpsPresent top[3]{};
    int top_count = 0;
    const DWORD* ids = fps_family_.empty() ? nullptr : fps_family_.data();
    fps_mon_.Sample(ids, fps_family_.size(), fps, status, source, top, 3, top_count);
    if (fps_ok_ && status == 4) {
        fps_ok_ = false;
        fps_retry_at_ = now + kPdhRetryMs;
    } else if (tried && !fps_ok_) {
        fps_retry_at_ = status == 2 ? ~0ull : now + kPdhRetryMs;
    }
    s.fps = fps;
    s.fps_status = status;
    if (status == 1 && source != 0) {
        if (source == root) s.fps_process = fps_name_;
        else if (!fps_tree_.Lookup(source, s.fps_process)) s.fps_process = QueryImageBaseName(source);
        if (s.fps_process.empty()) s.fps_process = fps_name_;
    } else {
        s.fps_process = fps_name_;
    }
    const bool ready = std::isfinite(fps);
    if (status != fps_logged_status_ || ready != fps_logged_ready_ || root_changed) {
        std::wstring message = L"status " + std::to_wstring(fps_logged_status_) + L"->" +
            std::to_wstring(status) + L" ready=" + (ready ? L"1" : L"0") +
            L" full=" + (detail_ ? L"1" : L"0") + L" root=" + std::to_wstring(root) +
            L" source=" + std::to_wstring(source) + L" family=" + std::to_wstring(fps_family_.size()) +
            L" process=" + s.fps_process + L" error=" + std::to_wstring(fps_mon_.LastError());
        if (top_count > 0) message += L" top=" + std::to_wstring(top[0].pid) +
            L":" + std::to_wstring(top[0].frames);
        diagnostics::WriteLog(L"FPS", message);
        fps_logged_status_ = status;
        fps_logged_ready_ = ready;
    }
    s.fps_top.clear();
    for (int i = 0; i < top_count; ++i) {
        FpsRank rank;
        rank.pid = top[i].pid;
        rank.frames = top[i].frames;
        if (!fps_tree_.Lookup(top[i].pid, rank.name)) rank.name = QueryImageBaseName(top[i].pid);
        if (rank.name.empty()) rank.name = std::to_wstring(static_cast<unsigned long long>(top[i].pid));
        s.fps_top.push_back(std::move(rank));
    }
}

void Metrics::Impl::UpdateProcs(Snapshot& s, unsigned long long now) {
    if (now < procs_due_) {
        s.top_procs = top_cache_;
        s.self_cpu_percent = self_cache_;
        s.process_count = process_count_;
        return;
    }
    procs_due_ = now + 2000;
    procs_buffers_ = true;
    diagnostics::ScopedTiming timing(L"Process scan");
    int count = -1;
    if (!procs_.Scan(top_cache_, self_cache_, count)) {
        top_cache_.clear();
        self_cache_ = NAN;
    }
    process_count_ = count;
    s.top_procs = top_cache_;
    s.self_cpu_percent = self_cache_;
    s.process_count = process_count_;
}

Snapshot Metrics::Impl::Take(bool full) {
    Wants wants = CurrentWants();
    if (!full) { wants.processes = false; wants.power_scheme = false; wants.dgpu_power = false; }
    if (!wants.chip_power) ReleaseEnergy();
    if (!wants.gpu_usage) ReleaseGpu();
    if (!wants.processes) ReleaseProcs();
    if (!wants.disk_io) ReleaseDisk();
    if (!wants.cpu_freq) ReleaseCpuFreq();
    if (!wants.gpu_memory) ReleaseGpuMem();
    if (!wants.gpu_temp) ReleaseGpuTemp();
    if (!wants.dgpu_power && (dgpu_.gate.api.module || dgpu_.gate.retry_at || dgpu_.last_gate != DgpuStatus::Off ||
        dgpu_.logged_status != DgpuStatus::Off)) dgpu_.Reset();
    if (!wants.wifi) ReleaseWifi();
    if (!wants.sys_disk) ReleaseSysDisk();
    if (!wants.battery_health) ReleaseBatteryHealth();
    if (!wants.fps) ReleaseFps();
    const bool reset = reset_intervals_.exchange(false, std::memory_order_acq_rel);
    if (detail_ != full || reset) {
        detail_ = full;
        procs_.Reset();
        top_cache_.clear();
        self_cache_ = NAN;
        process_count_ = -1;
        procs_due_ = 0;
        // Switch back to the compact Power-only query, or establish fresh full
        // Energy/GPU baselines. No differences may cross a mode/pause boundary.
        ReleaseEnergy();
        ReleaseGpu();
        have_net_ = have_cpu_ = have_tick_ = false;
        disk_warmed_ = cpu_freq_warmed_ = cpu_utility_warmed_ = false;
        have_disk_due_ = have_cpu_query_tick_ = false;
        if (!full && pdh_buf_.size() > 65536) std::vector<unsigned char>(65536).swap(pdh_buf_);
    }
    if (paused_.load(std::memory_order_relaxed)) {
        // A hidden widget must not keep a system-wide Present consumer.
        ReleaseFps(L"paused");
        dgpu_.Reset();
        last_.dgpu_status = DgpuStatus::Off;
        last_.dgpu_w = NAN;
        ClearTurnedOff(last_, wants);
        last_.fps = NAN;
        last_.fps_status = 0;
        last_.fps_process.clear();
        last_.fps_top.clear();
        const auto network = probe.Get();
        last_.net_state = network.state;
        last_.latency_ms = network.latency_ms;
        last_.offline_since_unix = network.offline_since_unix;
        last_.offline_by_latency = network.offline_by_latency;
        last_.offline_reason = network.offline_reason;
        last_.offline_latency_ms = network.offline_latency_ms;
        return last_;
    }
    diagnostics::ScopedTiming timing(L"Sample total (includes stages)");
    const unsigned long long now = GetTickCount64();
    double dt = 0;
    bool has_dt = false;
    if (have_tick_) {
        dt = static_cast<double>(now - last_tick_) / 1000.0;
        has_dt = true;
    }
    last_tick_ = now;
    have_tick_ = true;

    Snapshot s;
    UpdateNet(s, now);
    UpdateCpu(s, now, wants.cpu_freq);
    UpdateMem(s, wants.memory_detail);
    UpdateBattery(s, now, dt, has_dt);
    if (wants.power_scheme) {
        UpdateScheme(now);
        UpdateActivePlan(now);
        s.power_scheme = scheme_;
        s.power_scheme_kind = scheme_kind_;
        s.active_plan_valid = active_valid_;
        s.active_plan_name = active_name_;
        const GUID balanced = MakeGuid(0x381b4222, 0xf694, 0x41f0,
            0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e);
        s.active_plan_balanced = active_valid_ && have_active_guid_ && SameGuid(active_guid_, balanced);
    } else {
        scheme_ready_ = false;
        active_name_tick_ = 0;
    }
    if (wants.chip_power) {
        UpdateEnergy(full, now);
        s.pkg_w = pkg_w_;
        s.cpu_w = cpu_w_;
        s.gpu_w = gpu_w_;
        s.mem_w = mem_w_;
        s.pkg_power_from_energy = pkg_from_energy_;
        s.pkg_energy_delta_pwh = pkg_delta_pwh_;
        s.pkg_energy_interval_s = pkg_interval_s_;
        s.pkg_power_counter_w = pkg_power_counter_w_;
        s.pkg_meter = meter_status_[0];
        s.cpu_meter = meter_status_[1];
        s.gpu_meter = meter_status_[2];
        s.mem_meter = meter_status_[3];
    }
    if (wants.gpu_usage) {
        UpdateGpu(full, now);
        s.gpu_percent = gpu_percent_;
    }
    if (full && wants.processes) UpdateProcs(s, GetTickCount64());
    if (wants.process_count && !wants.processes) {
        // K32 lives in kernel32; no additional self-test link dependency.
        using PerformanceFn = BOOL(WINAPI*)(PPERFORMANCE_INFORMATION, DWORD);
        static const auto performance = reinterpret_cast<PerformanceFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetPerformanceInfo"));
        PERFORMANCE_INFORMATION info{};
        info.cb = sizeof(info);
        if (performance && performance(&info, sizeof(info))) s.process_count = static_cast<int>(info.ProcessCount);
    }
    if (wants.disk_io) UpdateDisk(now, s);
    if (wants.sys_disk) UpdateSysDisk(now, s);
    if (wants.uptime) UpdateUptime(s);
    if (wants.gpu_memory) UpdateGpuMemory(now, s);
    if (wants.gpu_temp) UpdateGpuTemp(now, s);
    if (full && wants.dgpu_power) {
        const auto result = dgpu_.Read(now);
        s.dgpu_status = result.status;
        s.dgpu_w = result.watts;
    }
    if (wants.wifi) UpdateWifi(now, s);
    if (wants.battery_health) UpdateBatteryHealth(now, s);
    if (wants.fps) UpdateFps(s, now);

    NetProbe::View view;
    {
        diagnostics::ScopedTiming view_timing(L"Network view (no HTTP)");
        view = probe.Get();
    }
    s.net_state = view.state;
    s.latency_ms = view.latency_ms;
    s.offline_since_unix = view.offline_since_unix;
    s.offline_by_latency = view.offline_by_latency;
    s.offline_reason = view.offline_reason;
    s.offline_latency_ms = view.offline_latency_ms;
    last_ = s;
    return s;
}

Metrics::Metrics() : impl_(new Impl) {}

Metrics::~Metrics() { delete impl_; }

void Metrics::Start(const Settings& s, EventCallback cb, void* user) { impl_->probe.Start(s, cb, user); }

Snapshot Metrics::Sample(bool full) { return impl_->Take(full); }

void Metrics::SetWants(const Wants& w) { impl_->ApplyWants(w); }

void Metrics::SetPaused(bool paused) {
    const bool was_paused = impl_->paused_.exchange(paused, std::memory_order_relaxed);
    if (was_paused && !paused) impl_->reset_intervals_.store(true, std::memory_order_release);
}

std::wstring Metrics::LogPath() const { return impl_->probe.LogPath(); }

void Metrics::SetProbeEnabled(bool enabled) { impl_->probe.SetEnabled(enabled); }

void Metrics::SetSlowThreshold(int slow_ms) { impl_->probe.SetSlowThreshold(slow_ms); }

void Metrics::OnSuspend() { impl_->probe.OnSuspend(); }

void Metrics::OnResume() {
    impl_->dgpu_.Reset();
    impl_->reset_intervals_.store(true, std::memory_order_release);
    impl_->probe.OnResume();
}

namespace {

std::wstring DiagStatus(long status) {
    wchar_t buffer[48]{};
    std::swprintf(buffer, 48, L"%ld (0x%08lX)", status, static_cast<unsigned long>(status));
    return buffer;
}

std::wstring DiagGuid(const GUID& guid) {
    wchar_t buffer[64]{};
    std::swprintf(buffer, 64, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        static_cast<unsigned long>(guid.Data1), static_cast<unsigned>(guid.Data2),
        static_cast<unsigned>(guid.Data3), guid.Data4[0], guid.Data4[1], guid.Data4[2],
        guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buffer;
}

std::wstring DiagVersion() {
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, 1024);
    if (!length || length >= 1024) return L"unknown";
    HMODULE library = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return L"unknown";
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using ReadFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto sizeFn = reinterpret_cast<SizeFn>(GetProcAddress(library, "GetFileVersionInfoSizeW"));
    const auto readFn = reinterpret_cast<ReadFn>(GetProcAddress(library, "GetFileVersionInfoW"));
    const auto queryFn = reinterpret_cast<QueryFn>(GetProcAddress(library, "VerQueryValueW"));
    std::wstring version = L"unknown";
    DWORD handle = 0;
    const DWORD bytes = sizeFn ? sizeFn(path, &handle) : 0;
    if (bytes > 0 && bytes < (4u << 20) && readFn && queryFn) {
        std::vector<unsigned char> block(bytes);
        if (readFn(path, 0, bytes, block.data())) {
            struct Head { DWORD signature, structVersion, fileMs, fileLs; };
            void* value = nullptr;
            UINT valueLen = 0;
            if (queryFn(block.data(), L"\\", &value, &valueLen) && value && valueLen >= sizeof(Head)) {
                Head head{};
                std::memcpy(&head, value, sizeof(head));
                if (head.signature == 0xFEEF04BDu) {
                    wchar_t text[64]{};
                    std::swprintf(text, 64, L"%u.%u.%u.%u",
                        static_cast<unsigned>(HIWORD(head.fileMs)), static_cast<unsigned>(LOWORD(head.fileMs)),
                        static_cast<unsigned>(HIWORD(head.fileLs)), static_cast<unsigned>(LOWORD(head.fileLs)));
                    version = text;
                }
            }
        }
    }
    FreeLibrary(library);
    return version;
}

const wchar_t* TraceName(PowerTrace trace) {
    switch (trace) {
    case PowerTrace::Package: return L"package";
    case PowerTrace::BatteryWhole: return L"battery-whole";
    case PowerTrace::BatterySide: return L"battery-side";
    default: return L"unknown";
    }
}

struct DiagRaw {
    std::wstring name;
    long status = 0;
    long long first = 0;
    long long second = 0;
    double power_mw = NAN; // Preserve formatted double, including NaN/Inf; never cast to integer.
};

bool CollectRaw(PDH_HCOUNTER counter, std::vector<DiagRaw>& out, long& array_status) {
    out.clear();
    array_status = PDH_INVALID_HANDLE;
    if (!counter) {
        return false;
    }
    std::vector<unsigned char> buffer(65536);
    DWORD count = 0;
    bool ok = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD need = static_cast<DWORD>(buffer.size());
        count = 0;
        const PDH_STATUS status = PdhGetRawCounterArrayW(counter, &need, &count,
            reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(buffer.data()));
        array_status = status;
        if (status == ERROR_SUCCESS) { ok = true; break; }
        if (static_cast<DWORD>(status) != static_cast<DWORD>(PDH_MORE_DATA) || need == 0 || need > (16u << 20))
            return false;
        buffer.resize(std::max(static_cast<std::size_t>(need), buffer.size() * 2));
    }
    if (!ok) return false;
    const auto* items = reinterpret_cast<const PDH_RAW_COUNTER_ITEM_W*>(buffer.data());
    count = std::min(count, static_cast<DWORD>(buffer.size() / sizeof(PDH_RAW_COUNTER_ITEM_W)));
    for (DWORD i = 0; i < count; ++i) {
        DiagRaw row;
        row.name = items[i].szName ? items[i].szName : L"";
        row.status = items[i].RawValue.CStatus;
        row.first = items[i].RawValue.FirstValue;
        row.second = items[i].RawValue.SecondValue;
        out.push_back(std::move(row));
    }
    return true;
}

}  // namespace

std::wstring BuildDgpuGateDiagnosticsReport() {
    DgpuGate gate;
    const auto status = gate.Read(GetTickCount64());
    return DgpuGateReport(gate, status); // CfgMgr only; never loads NVML.
}

std::wstring BuildDgpuDiagnosticsReport(bool allow_wake, void (*before_nvml)(const std::wstring&)) {
    std::wstring report;
    auto line = [&](const std::wstring& text) { report += text; report += L"\r\n"; };
    DgpuGate gate;
    const DgpuStatus status = gate.Read(GetTickCount64());
    line(L"[NVIDIA dGPU]");
    report += DgpuGateReport(gate, status);
    line(L"Only system-reported D0 opens the normal NVML gate; temperature is not used.");
    if (status != DgpuStatus::Valid && !allow_wake) {
        if (status == DgpuStatus::Sleeping) line(L"asleep; NVML not called to avoid waking it");
        else if (status == DgpuStatus::NoNvidia) line(L"No NVIDIA display adapter; NVML not called to avoid waking it");
        else line(L"CfgMgr gate failed or unknown; NVML not called to avoid waking it");
        return report;
    }
    if (status != DgpuStatus::Valid)
        line(L"WARNING: dGPU looks asleep; NVML may wake it.");
    if (before_nvml) { before_nvml(report); report.clear(); }
    NvmlApi nvml;
    const bool opened = nvml.Open();
    line(L"NVML path: " + nvml.path);
    line(L"NVML load_error=" + std::to_wstring(nvml.load_error));
    line(L"nvmlInit_v2: " + std::to_wstring(nvml.init_result) +
        (nvml.init_result == -1 ? L" (not called)" : L""));
    if (!opened) { line(L"Result: NvmlUnavailable"); return report; }
    unsigned count = 0;
    const nvmlReturn_t count_rc = nvml.count(&count);
    line(L"nvmlDeviceGetCount_v2: " + std::to_wstring(count_rc) + L" devices=" + std::to_wstring(count));
    if (count_rc != kNvmlSuccess || count > 4096) { line(L"Result: ReadFailed"); return report; }
    std::vector<DgpuDevice> readings;
    bool failed = false;
    for (unsigned i = 0; i < count; ++i) {
        nvmlDevice_t handle = nullptr;
        const nvmlReturn_t handle_rc = nvml.handle(i, &handle);
        line(L"  device=" + std::to_wstring(i) + L" handle_rc=" + std::to_wstring(handle_rc));
        if (handle_rc != kNvmlSuccess || !handle) { failed = true; continue; }
        nvmlPciInfo_t pci{};
        const nvmlReturn_t pci_rc = nvml.pci(handle, &pci);
        char name[128]{};
        const nvmlReturn_t name_rc = nvml.name(handle, name, static_cast<unsigned>(sizeof(name)));
        name[sizeof(name) - 1] = '\0';
        wchar_t wide_name[128]{};
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 128);
        line(L"    name_rc=" + std::to_wstring(name_rc) + L" name=" + wide_name);
        wchar_t text[256]{};
        std::swprintf(text, 256, L"    pci_rc=%d PCI=%u:%u:%u (domain:bus:device)", pci_rc,
            pci.domain, pci.bus, pci.device);
        line(text);
        if (pci_rc != kNvmlSuccess) { failed = true; if (!allow_wake) continue; }
        const bool matched = pci_rc == kNvmlSuccess && pci.domain == 0 &&
            DgpuMatches(gate.adapters, pci.bus, pci.device);
        line(std::wstring(L"    awake_match=") + (matched ? L"yes" : L"no"));
        if (!allow_wake && !matched) { line(L"    PowerUsage skipped: adapter is not an awake match"); continue; }
        unsigned limit_mw = 0;
        const nvmlReturn_t limit_rc = nvml.enforced_limit ? nvml.enforced_limit(handle, &limit_mw) : -1;
        line(L"    nvmlDeviceGetEnforcedPowerLimit: " + std::to_wstring(limit_rc) +
            L" mW=" + std::to_wstring(limit_mw));
        if (limit_rc != kNvmlSuccess) limit_mw = 0;
        unsigned mw = 0;
        nvmlReturn_t power_rc;
        {
            diagnostics::ScopedTiming timing(L"NVML power");
            power_rc = nvml.power(handle, &mw);
        }
        line(L"    nvmlDeviceGetPowerUsage: " + std::to_wstring(power_rc) + L" mW=" + std::to_wstring(mw));
        if (matched) readings.push_back({pci.bus, pci.device, power_rc, mw, limit_mw, limit_rc == kNvmlSuccess});
    }
    const DgpuResult result = InterpretDgpu(gate.adapters, readings);
    line(std::wstring(L"Result: ") + (failed ? L"ReadFailed" : DgpuStatusName(result.status)) +
        L" awake_matched=" + std::to_wstring(result.matched));
    if (!failed && result.status == DgpuStatus::Valid) line(L"dGPU watts: " + std::to_wstring(result.watts));
    line(L"nvmlShutdown + FreeLibrary on exit");
    return report;
}

std::wstring BuildPowerDiagnosticsReport() {
    std::wstring report;
    auto line = [&](const std::wstring& text) { report += text; report += L"\r\n"; };
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[64]{};
    std::swprintf(stamp, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    wchar_t modulePath[1024]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, 1024);
    modulePath[1023] = L'\0';
    line(L"Pulse power diagnostics");
    line(std::wstring(L"Time: ") + stamp);
    line(std::wstring(L"Version: ") + DiagVersion());
    line(std::wstring(L"Module: ") + (moduleLength && moduleLength < 1024 ? modulePath : L"unavailable (path failed or truncated)"));
    line(L"Units: Energy counter raw value = picowatt-hours (pWh). watts = (delta pWh) * 3.6e-9 / QPC seconds.");
    line(L"Units: Power counter = milliwatts (mW). watts = mW / 1000.");
    line(L"D3DKMT_ADAPTER_PERFDATA.Power is 0.1 percent, not watts, and is not used as a fallback.");
    line(L"D3DKMT_ADAPTER_PERFDATA.FanRPM is the adapter fan, not a general CPU fan, and is not read here.");
    line(L"Domains: package=package0_pkg, cpu=pp0, graphics=pp1, memory=dram. pp1 is an EMI graphics domain, not discrete-GPU board power.");
    line(L"GPU Engine utilization can come from a different adapter than pp1. NVIDIA board power uses NVML only after a system-reported D0 CfgMgr gate; ADLX is not used.");
    line(L"");

    HMODULE powerLib = GetModuleHandleW(L"powrprof.dll");
    const bool loadedPower = powerLib == nullptr;
    if (!powerLib) powerLib = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto activeFn = powerLib ? reinterpret_cast<ActiveSchemeFn>(GetProcAddress(powerLib, "PowerGetActiveScheme")) : nullptr;
    const auto friendlyFn = powerLib ? reinterpret_cast<FriendlyNameFn>(GetProcAddress(powerLib, "PowerReadFriendlyName")) : nullptr;
    const auto overlayFn = powerLib ? reinterpret_cast<OverlayFn>(GetProcAddress(powerLib, "PowerGetEffectiveOverlayScheme")) : nullptr;
    line(L"[Power plan]");
    GUID* active = nullptr;
    const DWORD activeStatus = activeFn ? activeFn(nullptr, &active) : ERROR_PROC_NOT_FOUND;
    line(std::wstring(L"PowerGetActiveScheme: ") + DiagStatus(static_cast<long>(activeStatus)));
    if (activeStatus == ERROR_SUCCESS && active) {
        const GUID guid = *active;
        LocalFree(active);
        line(std::wstring(L"Active scheme GUID: ") + DiagGuid(guid));
        std::wstring plan;
        if (ReadPlanName(friendlyFn, guid, plan)) line(std::wstring(L"Friendly name: ") + plan);
        else line(L"Friendly name: unavailable (query failed; no previous name is kept)");
    } else {
        if (active) LocalFree(active);
        line(L"Friendly name: unavailable");
    }
    GUID overlay{};
    const DWORD overlayStatus = overlayFn ? overlayFn(&overlay) : ERROR_PROC_NOT_FOUND;
    line(std::wstring(L"PowerGetEffectiveOverlayScheme: ") + DiagStatus(static_cast<long>(overlayStatus)));
    if (overlayStatus == ERROR_SUCCESS) {
        line(std::wstring(L"Overlay GUID: ") + DiagGuid(overlay));
        line(std::wstring(L"Overlay kind: ") + SchemeName(SchemeKind(overlay)));
    }
    line(L"The overlay is a performance preference. It is not the Control Panel power plan.");
    line(L"");

    line(L"[Battery]");
    BatteryReading battery{};
    const bool batteryOk = ReadBattery(battery);
    line(batteryOk ? L"Read: success" : L"Read: failed; AC line and rate are not confirmed");
    if (batteryOk) {
        BatterySample sample;
        sample.read_ok = true;
        sample.present = battery.present;
        sample.ac_known = battery.ac_known;
        sample.ac_online = battery.ac;
        sample.charging = battery.charging;
        sample.discharging = battery.discharging;
        sample.rate_known = battery.rate_known;
        sample.rate_mw = battery.rate_mw;
        const BatteryClassification classified = ClassifyBattery(sample);
        line(std::wstring(L"Present: ") + (battery.present ? L"yes" : L"no"));
        line(std::wstring(L"AC known/online: ") + (classified.ac_known ? L"yes" : L"no") +
            L" / " + (classified.ac_online ? L"online" : L"offline"));
        line(std::wstring(L"Charging flag: ") + (battery.charging ? L"1" : L"0"));
        line(std::wstring(L"Discharging flag: ") + (battery.discharging ? L"1" : L"0"));
        if (battery.rate_known) {
            wchar_t rate[64]{};
            std::swprintf(rate, 64, L"%.3f mW (signed; positive=charge, negative=discharge)", battery.rate_mw);
            line(std::wstring(L"Rate: ") + rate);
        } else line(L"Rate: unknown (not treated as 0 W)");
        line(std::wstring(L"Classified trace: ") + TraceName(classified.trace));
        line(std::wstring(L"Supplemental: ") + (classified.supplemental ? L"yes" : L"no"));
        line(std::wstring(L"Pure battery: ") + (classified.pure_battery ? L"yes" : L"no"));
        line(std::wstring(L"Contested: ") + (classified.contested ? L"yes" : L"no"));
        line(L"Pure battery requires known offline AC and uncontested discharge. AC plus battery discharge is supplemental, not whole-machine power.");
    }
    line(L"");

    report += BuildDgpuDiagnosticsReport(false);
    line(L"");
    line(L"[Energy Meter]");
    PDH_HQUERY query = nullptr;
    const PDH_STATUS openStatus = PdhOpenQueryW(nullptr, 0, &query);
    line(std::wstring(L"PdhOpenQuery: ") + DiagStatus(openStatus));
    PDH_HCOUNTER energy = nullptr, power = nullptr, meterTime = nullptr;
    PDH_STATUS energyAdd = openStatus, powerAdd = openStatus, timeAdd = openStatus;
    if (openStatus == ERROR_SUCCESS) {
        energyAdd = PdhAddEnglishCounterW(query, L"\\Energy Meter(*)\\Energy", 0, &energy);
        powerAdd = PdhAddEnglishCounterW(query, L"\\Energy Meter(*)\\Power", 0, &power);
        timeAdd = PdhAddEnglishCounterW(query, L"\\Energy Meter(*)\\Time", 0, &meterTime);
    }
    if (energyAdd != ERROR_SUCCESS) energy = nullptr;
    if (powerAdd != ERROR_SUCCESS) power = nullptr;
    line(std::wstring(L"Add Energy (pWh): ") + DiagStatus(energyAdd));
    line(std::wstring(L"Add Power (mW): ") + DiagStatus(powerAdd));
    line(std::wstring(L"Add Time: ") + DiagStatus(timeAdd));
    std::vector<DiagRaw> firstRaw, secondRaw, secondPower;
    long firstArray = energyAdd, secondArray = energyAdd, powerArray = powerAdd;
    PDH_STATUS firstCollect = openStatus == ERROR_SUCCESS ? PdhCollectQueryData(query) : openStatus;
    line(std::wstring(L"Collect 1: ") + DiagStatus(firstCollect));
    const std::int64_t qpc1 = QpcNow();
    if (firstCollect == ERROR_SUCCESS && energy) {
        CollectRaw(energy, firstRaw, firstArray);
    } else if (firstCollect != ERROR_SUCCESS) firstArray = firstCollect;
    line(std::wstring(L"Energy array 1 (add/collect status if skipped): ") + DiagStatus(firstArray));
    if (openStatus == ERROR_SUCCESS) Sleep(1000);
    PDH_STATUS secondCollect = openStatus == ERROR_SUCCESS ? PdhCollectQueryData(query) : openStatus;
    const std::int64_t qpc2 = QpcNow();
    line(std::wstring(L"Collect 2: ") + DiagStatus(secondCollect));
    const double seconds = QpcSeconds(qpc2, qpc1);
    {
        wchar_t gap[64]{};
        std::swprintf(gap, 64, L"QPC interval: %.3f s", seconds);
        line(gap);
    }
    if (secondCollect == ERROR_SUCCESS) {
        if (energy) CollectRaw(energy, secondRaw, secondArray);
        std::vector<unsigned char> formatted(65536);
        DWORD count = 0;
        if (power && FormatArray(power, formatted, count, PDH_FMT_DOUBLE, &powerArray)) {
            const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(formatted.data());
            count = std::min(count, static_cast<DWORD>(formatted.size() / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W)));
            for (DWORD i = 0; i < count; ++i) {
                DiagRaw row;
                row.name = items[i].szName ? items[i].szName : L"";
                row.status = items[i].FmtValue.CStatus;
                row.power_mw = items[i].FmtValue.doubleValue;
                secondPower.push_back(std::move(row));
            }
        }
    } else {
        secondArray = powerArray = secondCollect;
    }
    line(std::wstring(L"Energy array 2 (add/collect status if skipped): ") + DiagStatus(secondArray));
    line(std::wstring(L"Power array (add/collect status if skipped): ") + DiagStatus(powerArray));
    auto listEnergy = [&](const wchar_t* title, const std::vector<DiagRaw>& rows) {
        line(title);
        if (rows.empty()) line(L"  (no rows; consult add/collect/array status above)");
        for (const auto& row : rows) {
            wchar_t text[256]{};
            std::swprintf(text, 256, L"  status=%ld first=%lld second=%lld name=",
                row.status, row.first, row.second);
            line(std::wstring(text) + row.name);
        }
    };
    listEnergy(L"Raw Energy instances (sample 1, pWh):", firstRaw);
    listEnergy(L"Raw Energy instances (sample 2, pWh):", secondRaw);
    line(L"Power instances (formatted double, mW):");
    if (secondPower.empty()) line(L"  (no rows; consult add/collect/array status above)");
    for (const auto& row : secondPower) {
        wchar_t text[128]{};
        std::swprintf(text, 128, L"  status=%ld mW=%.6g name=", row.status, row.power_mw);
        line(std::wstring(text) + row.name);
    }
    const wchar_t* domainName[] = {L"package", L"cpu", L"graphics", L"memory"};
    bool graphics_match = false;
    std::array<unsigned, 6> outcomes{};
    // Prefer a valid matching row, then the same domain score used by normal sampling.
    auto choose = [&](const std::vector<DiagRaw>& rows, std::size_t domain, bool formatted) {
        const DiagRaw* chosen = nullptr;
        int best = 0;
        for (const auto& row : rows) {
            int score = EnergyScore(row.name.c_str(), domain);
            if (!score) continue;
            if (MeterDataValid(row.status) && (!formatted || MeterPowerValid(row.power_mw))) score += 10;
            if (score > best) { best = score; chosen = &row; }
        }
        return chosen;
    };
    line(L"Matched domains:");
    for (std::size_t domain = 0; domain < 4; ++domain) {
        const DiagRaw* after = choose(secondRaw, domain, false);
        const DiagRaw* before = after ? nullptr : choose(firstRaw, domain, false);
        if (after) {
            for (const auto& row : firstRaw)
                if (row.name == after->name) { before = &row; break; }
        }
        const DiagRaw* fallback = choose(secondPower, domain, true);
        const MeterEvidence e{energy != nullptr, secondArray == ERROR_SUCCESS, before || after};
        const MeterEvidence p{power != nullptr, powerArray == ERROR_SUCCESS, fallback != nullptr};
        MeterEnergySample b, a;
        if (before) b = {before->name, static_cast<std::uint32_t>(before->status), static_cast<std::uint64_t>(before->first)};
        if (after) a = {after->name, static_cast<std::uint32_t>(after->status), static_cast<std::uint64_t>(after->first)};
        const auto result = InterpretMeterDomain(e, p,
            firstArray == ERROR_SUCCESS && before ? &b : nullptr, after ? &a : nullptr, seconds,
            fallback ? static_cast<std::uint32_t>(fallback->status) : static_cast<std::uint32_t>(PDH_INVALID_DATA),
            fallback ? fallback->power_mw : NAN);
        ++outcomes[static_cast<std::size_t>(result.status)];
        if (domain == 2) graphics_match = e.matched || p.matched;
        line(std::wstring(L"  ") + domainName[domain] + L":");
        if (after || before) line(L"    Energy name: " + (after ? after->name : before->name));
        if (fallback) line(L"    Power name: " + fallback->name);
        if (std::isfinite(result.energy_w)) {
            wchar_t text[160]{};
            std::swprintf(text, 160, L"    Energy: delta_pWh=%llu -> %.3f W",
                static_cast<unsigned long long>(result.delta_pwh), result.energy_w);
            line(text);
        } else if (result.interval == MeterInterval::InvalidData) {
            line(L"    Energy: invalid CStatus; no delta calculated (see raw rows).");
        } else if (result.interval == MeterInterval::Unchanged) {
            line(L"    Energy: unchanged; warmup/stale provider, not evidence of 0 W.");
        } else if (result.interval == MeterInterval::Warmup || result.interval == MeterInterval::Reset) {
            line(L"    Energy: warmup/reset/identity change; no valid same-name interval yet.");
        } else line(L"    Energy: unavailable; consult add/collect/array status and instance names.");
        if (std::isfinite(result.power_w)) {
            wchar_t text[160]{};
            std::swprintf(text, 160, L"    Power: %.6g mW / 1000 = %.3f W%ls", fallback->power_mw,
                result.power_w, std::isfinite(result.energy_w) ? L" (reference)" : L" (fallback available)");
            line(text);
        } else line(L"    Power: unavailable; consult status, CStatus and value above.");
        switch (result.status) {
        case MeterStatus::Valid: line(L"    Result: valid source available."); break;
        case MeterStatus::Warmup: line(L"    Result: warmup/unchanged; retry later."); break;
        case MeterStatus::CounterUnavailable: line(L"    Result: counters unavailable."); break;
        case MeterStatus::NoMatch: line(L"    Result: arrays read successfully, no matching domain name."); break;
        default: line(L"    Result: read failed or invalid data; not evidence of missing hardware."); break;
        }
    }
    if (query) PdhCloseQuery(query);
    if (loadedPower && powerLib) FreeLibrary(powerLib);
    line(L"");
    line(L"[Why a value may be missing]");
    if (openStatus != ERROR_SUCCESS) {
        line(L"The PDH query could not be opened. This sample does not prove the hardware can never report power.");
    } else if (energyAdd != ERROR_SUCCESS && powerAdd != ERROR_SUCCESS) {
        line(L"Neither the Energy nor the Power counter could be added. Energy Meter data is unavailable on this boot.");
        line(L"That usually means the EMI device did not expose the counter. It does not by itself prove every other interface is impossible.");
    } else {
        line(L"Domain counts: valid=" + std::to_wstring(outcomes[static_cast<std::size_t>(MeterStatus::Valid)]) +
            L", warmup/unchanged=" + std::to_wstring(outcomes[static_cast<std::size_t>(MeterStatus::Warmup)]) +
            L", read failed/invalid=" + std::to_wstring(outcomes[static_cast<std::size_t>(MeterStatus::ReadFailed)]) +
            L", no matching name=" + std::to_wstring(outcomes[static_cast<std::size_t>(MeterStatus::NoMatch)]));
        line(L"Energy and Power were matched independently. A valid Power-only domain is a usable mW-to-W fallback.");
        line(L"An add, collect, array or CStatus failure is not proof that an instance or hardware is absent.");
    }
    if (graphics_match) {
        line(L"A pp1 match is EMI graphics-domain data. It is not the board power of an RTX or other discrete GPU, and it may not be the adapter shown by GPU usage.");
    } else {
        line(L"No pp1 name was observed in the returned Energy/Power rows; if a read failed, presence is unknown. GPU usage can still be available from GPU Engine.");
    }
    line(L"Normal UI keeps unknown numbers as --. It does not substitute 0 W, D3DKMT Power, or an estimated total.");
    return report;
}

int WritePowerDiagnosticsFile(std::wstring* path_out, std::wstring* error_out) {
    auto fail = [&](const std::wstring& message, int code) {
        if (error_out) *error_out = message;
        return code;
    };
    wchar_t local[MAX_PATH]{};
    const DWORD localLen = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (localLen == 0 || localLen >= MAX_PATH) return fail(L"LOCALAPPDATA is unavailable.", 2);
    const std::wstring directory = std::wstring(local) + L"\\Pulse";
    const std::wstring path = directory + L"\\power-diagnostics.txt";
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            return fail(L"Could not create the Pulse directory. Win32 error " + std::to_wstring(error), 2);
    }
    std::wstring report;
    try {
        report = BuildPowerDiagnosticsReport();
    } catch (...) {
        return fail(L"Collecting the diagnostic failed.", 3);
    }
    if (report.empty()) return fail(L"The diagnostic report was empty.", 3);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), static_cast<int>(report.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return fail(L"Could not encode the diagnostic as UTF-8.", 3);
    std::string encoded(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, report.c_str(), static_cast<int>(report.size()),
        encoded.data(), bytes, nullptr, nullptr) != bytes)
        return fail(L"Could not encode the complete diagnostic as UTF-8.", 3);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fail(L"Could not write power-diagnostics.txt. Win32 error " + std::to_wstring(GetLastError()), 2);
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD bomWritten = 0, bodyWritten = 0;
    const bool bomOk = WriteFile(file, bom, sizeof(bom), &bomWritten, nullptr) && bomWritten == sizeof(bom);
    const bool bodyOk = bomOk && WriteFile(file, encoded.data(), static_cast<DWORD>(encoded.size()), &bodyWritten, nullptr) &&
        bodyWritten == static_cast<DWORD>(encoded.size());
    CloseHandle(file);
    if (!bomOk || !bodyOk) return fail(L"Writing power-diagnostics.txt failed.", 2);
    if (path_out) *path_out = path;
    if (error_out) error_out->clear();
    return 0;
}

}  // namespace pulse
