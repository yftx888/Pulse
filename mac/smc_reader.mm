#include "smc_reader.h"
namespace pulse { namespace mac {
bool SmcReader::ReadFloat(std::uint32_t key, double& value, kern_return_t* kernel_status) {
    if (kernel_status) *kernel_status = KERN_SUCCESS;
    const auto& info = Info(key);
    if (kernel_status) *kernel_status = info.call.kernel;
    if (!info.call.ok || info.info.dataType != Code("flt ") || info.info.dataSize != 4) return false;
    const auto reading = Read(key, info);
    if (kernel_status) *kernel_status = reading.call.kernel;
    if (!reading.call.ok) return false;
    float f = 0;
    std::memcpy(&f, reading.bytes.data(), sizeof(f)); // arm64 little-endian float32.
    if (!std::isfinite(f)) return false;
    value = f;
    return true;
}
bool SmcReader::ReadInt32LE(std::uint32_t key, std::int32_t& value, kern_return_t* kernel_status) {
    if (kernel_status) *kernel_status = KERN_SUCCESS;
    const auto& info = Info(key);
    if (kernel_status) *kernel_status = info.call.kernel;
    if (!info.call.ok || info.info.dataType != Code("si32") || info.info.dataSize != 4) return false;
    const auto reading = Read(key, info);
    if (kernel_status) *kernel_status = reading.call.kernel;
    if (!reading.call.ok) return false;
    std::int32_t signed_value = 0;
    std::memcpy(&signed_value, reading.bytes.data(), sizeof(signed_value)); // arm64 little-endian.
    value = signed_value;
    return true;
}
} }
