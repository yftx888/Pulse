#pragma once
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse { namespace mac {
static_assert(sizeof(wchar_t) == 4, "macOS UTF-32 wchar_t required");
// macOS wchar_t is UTF-32. Convert scalar values explicitly; never reinterpret UTF-16.
inline std::wstring ToWide(NSString* text) {
    std::wstring result;
    if (!text) return result;
    for (NSUInteger i = 0; i < text.length; ++i) {
        std::uint32_t c = [text characterAtIndex:i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < text.length) {
                const std::uint32_t next = [text characterAtIndex:i + 1];
                if (next >= 0xDC00 && next <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + next - 0xDC00;
                    ++i;
                } else c = 0xFFFD;
            } else c = 0xFFFD;
        } else if (c >= 0xDC00 && c <= 0xDFFF) c = 0xFFFD;
        result.push_back(static_cast<wchar_t>(c));
    }
    return result;
}
inline NSString* ToNSString(const std::wstring& text) {
    std::vector<unichar> units;
    units.reserve(text.size());
    for (wchar_t wc : text) {
        std::uint32_t c = static_cast<std::uint32_t>(wc);
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c < 0x10000) units.push_back(static_cast<unichar>(c));
        else {
            c -= 0x10000;
            units.push_back(static_cast<unichar>(0xD800 + (c >> 10)));
            units.push_back(static_cast<unichar>(0xDC00 + (c & 0x3FF)));
        }
    }
    return [NSString stringWithCharacters:units.data() length:units.size()];
}
inline std::wstring FromCF(CFStringRef text) { return ToWide((__bridge NSString*)text); }
inline std::string Utf8(const std::wstring& text) {
    NSData* data = [ToNSString(text) dataUsingEncoding:NSUTF8StringEncoding];
    return data.length ? std::string(static_cast<const char*>(data.bytes), data.length) : std::string();
}
inline std::wstring FromUtf8(const char* text) {
    return ToWide(text ? [NSString stringWithUTF8String:text] : nil);
}
} } // namespace pulse::mac
