#pragma once
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>

namespace pulse { namespace mac {
// Wire ABI: fixed-width fields and native C alignment, not packed structs.
// References: smcFanControl smc.h; Stats SMC/smc.swift; SMCKit SMCParamStruct.
struct SmcVersion {
    std::uint8_t major, minor, build, reserved;
    std::uint16_t release;
};
struct SmcLimits {
    std::uint16_t version, length;
    std::uint32_t cpuPLimit, gpuPLimit, memPLimit;
};
struct SmcKeyInfo {
    std::uint32_t dataSize, dataType;
    std::uint8_t dataAttributes;
};
struct SMCKeyData_t {
    std::uint32_t key;
    SmcVersion vers;
    SmcLimits pLimitData;
    SmcKeyInfo keyInfo;
    std::uint8_t result, status, data8;
    std::uint32_t data32;
    std::uint8_t bytes[32];
};
static_assert(sizeof(SmcVersion) == 6 && sizeof(SmcLimits) == 16 && sizeof(SmcKeyInfo) == 12,
              "SMC nested ABI mismatch");
static_assert(sizeof(SMCKeyData_t) == 80, "SMC user-client ABI must be 80 bytes");
static_assert(offsetof(SMCKeyData_t, pLimitData) == 12 && offsetof(SMCKeyData_t, keyInfo) == 28 &&
              offsetof(SMCKeyData_t, result) == 40 && offsetof(SMCKeyData_t, status) == 41 &&
              offsetof(SMCKeyData_t, data8) == 42 && offsetof(SMCKeyData_t, data32) == 44 &&
              offsetof(SMCKeyData_t, bytes) == 48, "SMC wire offsets mismatch");
static_assert(sizeof(float) == 4, "SMC flt requires float32");

constexpr std::uint32_t Code(const char (&text)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]));
}
inline std::wstring FourText(std::uint32_t code) {
    std::wostringstream out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const unsigned c = (code >> shift) & 0xff;
        if (c >= 0x20 && c <= 0x7e) out << static_cast<wchar_t>(c);
        else out << L"\\x" << std::hex << std::setw(2) << std::setfill(L'0') << c << std::dec;
    }
    return out.str();
}
inline double Seconds(std::chrono::steady_clock::time_point at, std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(at - start).count();
}
// This is the entire command set. The sole driver-call site also checks it.
enum class ReadCommand : std::uint8_t { Bytes = 5, Index = 8, Info = 9 };
struct CallResult {
    bool ok = false;
    kern_return_t kernel = KERN_SUCCESS;
    std::uint8_t result = 0, status = 0;
    std::size_t output_size = 0;
};
inline void PrintCall(std::wostream& out, const CallResult& call) {
    out << L"kern_return_t=" << call.kernel << L"(0x" << std::hex
        << static_cast<std::uint32_t>(call.kernel) << std::dec << L") result="
        << static_cast<unsigned>(call.result) << L" status=" << static_cast<unsigned>(call.status)
        << L" output_size=" << call.output_size;
}
struct CachedInfo { SmcKeyInfo info{}; CallResult call; };
struct Reading {
    std::array<std::uint8_t, 32> bytes{};
    CallResult call;
};
class SmcReader {
    io_service_t service_ = IO_OBJECT_NULL;
    io_connect_t connection_ = IO_OBJECT_NULL;
    std::map<std::uint32_t, CachedInfo> cache_;
    CallResult Call(ReadCommand command, SMCKeyData_t& input, SMCKeyData_t& reply) {
        CallResult call;
        switch (command) {
        case ReadCommand::Bytes: case ReadCommand::Index: case ReadCommand::Info: break;
        default: call.kernel = kIOReturnBadArgument; return call;
        }
        if (!connection_) { call.kernel = kIOReturnNotOpen; return call; }
        input.data8 = static_cast<std::uint8_t>(command);
        call.output_size = sizeof(reply);
        call.kernel = IOConnectCallStructMethod(connection_, 2, &input, sizeof(input), &reply, &call.output_size);
        call.result = reply.result; call.status = reply.status;
        call.ok = call.kernel == KERN_SUCCESS && call.output_size == sizeof(reply) && call.result == 0;
        return call;
    }
public:
    SmcReader() = default;
    SmcReader(const SmcReader&) = delete;
    SmcReader& operator=(const SmcReader&) = delete;
    ~SmcReader() { Close(); }
    void Close() {
        if (connection_) IOServiceClose(connection_);
        if (service_) IOObjectRelease(service_);
        connection_ = service_ = IO_OBJECT_NULL; cache_.clear();
    }
    // kernel_status only reports driver/connection failures; invalid values keep KERN_SUCCESS.
    bool ReadFloat(std::uint32_t key, double& value, kern_return_t* kernel_status = nullptr);
    // si32, 4 bytes, arm64 little-endian. Same kernel_status rules as ReadFloat.
    bool ReadInt32LE(std::uint32_t key, std::int32_t& value, kern_return_t* kernel_status = nullptr);
    bool Open(std::wostream& out) {
        if (connection_) return true;
        Close();
        CFMutableDictionaryRef matching = IOServiceMatching("AppleSMC");
        if (!matching) {
            out << L"SMC open=failed matching=null kern_return_t=" << kIOReturnNoMemory << L"（本地诊断码）\n";
            return false;
        }
        // GetMatchingService consumes matching, including on failure.
        service_ = IOServiceGetMatchingService(kIOMainPortDefault, matching);
        if (!service_) {
            out << L"SMC open=failed AppleSMC absent kern_return_t=" << kIOReturnNotFound
                << L"（匹配 API 不返回 kern_return_t，此为本地诊断码）\n";
            return false;
        }
        const kern_return_t status = IOServiceOpen(service_, mach_task_self(), 0, &connection_);
        out << L"SMC IOServiceOpen kern_return_t=" << status << L"(0x" << std::hex
            << static_cast<std::uint32_t>(status) << std::dec << L") connection=" << connection_
            << L" open=" << (status == KERN_SUCCESS && connection_ ? L"ok" : L"failed") << L"\n";
        return status == KERN_SUCCESS && connection_;
    }
    // A failed optional key must not invalidate the shared SMC connection.
    void ForgetInfo(std::uint32_t key) { cache_.erase(key); }
    const CachedInfo& Info(std::uint32_t key) {
        const auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        SMCKeyData_t input{}, reply{};
        input.key = key;
        CachedInfo entry;
        entry.call = Call(ReadCommand::Info, input, reply);
        if (entry.call.ok) entry.info = reply.keyInfo;
        return cache_.emplace(key, entry).first->second;
    }
    CallResult KeyAt(std::uint32_t index, std::uint32_t& key) {
        SMCKeyData_t input{}, reply{};
        input.data32 = index;
        const auto call = Call(ReadCommand::Index, input, reply);
        if (call.ok) key = reply.key;
        return call;
    }
    Reading Read(std::uint32_t key, const CachedInfo& info) {
        Reading value;
        if (!info.call.ok) { value.call = info.call; return value; }
        if (!info.info.dataSize || info.info.dataSize > value.bytes.size()) {
            value.call.kernel = kIOReturnBadArgument; return value;
        }
        SMCKeyData_t input{}, reply{};
        input.key = key; input.keyInfo.dataSize = info.info.dataSize;
        value.call = Call(ReadCommand::Bytes, input, reply);
        if (value.call.ok) std::memcpy(value.bytes.data(), reply.bytes, info.info.dataSize);
        return value;
    }
};
} }
