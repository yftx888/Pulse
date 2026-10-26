// HTTP status policy shared by the probe and directed tests.
#pragma once

namespace pulse {

inline bool ProbeHttpStatusOk(unsigned long code) {
    return code >= 200 && code < 300;
}

}  // namespace pulse
