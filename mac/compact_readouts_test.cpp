#include "compact_readouts.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace pulse;
using pulse::mac::MakeCompactReadouts;

int main() {
    unsigned checks=0;
    auto check=[&](const Snapshot& s, const wchar_t* watts, const wchar_t* time) {
        const auto r=MakeCompactReadouts(s);
        assert(r.watts==watts && r.remaining==time);
        ++checks;
    };
    Snapshot s;
    check(s,L"--W",L"--:--");
    s.mode=PowerMode::Discharging; s.power_trace=PowerTrace::BatteryWhole;
    s.ac_known=true; s.battery_rate_valid=true; s.power_w=5.2; s.eta_s=23400;
    check(s,L"5.2W",L"6:30");
    s.eta_s=2700; check(s,L"5.2W",L"0:45");
    s.eta_s=59; check(s,L"5.2W",L"0:00");
    s.eta_s=3599; check(s,L"5.2W",L"0:59");
    s.eta_s=3600; check(s,L"5.2W",L"1:00");
    s.eta_s=360000; check(s,L"5.2W",L"100:00");
    s.eta_s=NAN; check(s,L"5.2W",L"--:--");
    s.eta_s=-1; check(s,L"5.2W",L"--:--");
    s.eta_s=std::numeric_limits<double>::infinity(); check(s,L"5.2W",L"--:--");
    s.eta_s=23400; s.power_w=NAN; check(s,L"--W",L"6:30");
    s.power_w=-5.2; check(s,L"--W",L"6:30");
    s.power_w=0; check(s,L"--W",L"6:30");
    s.power_w=123.4; check(s,L"123.4W",L"6:30");
    s.power_w=5.2; s.battery_rate_valid=false; check(s,L"--W",L"--:--");
    s.eta_from_system=true; check(s,L"--W",L"6:30");
    s.battery_rate_valid=true; s.power_contested=true; check(s,L"--W",L"--:--");
    s.power_contested=false; s.ac_known=false; check(s,L"--W",L"--:--");
    s.ac_known=true; s.ac_online=true; s.mode=PowerMode::Charging;
    s.power_trace=PowerTrace::Package; s.system_w=18.2;
    check(s,L"18.2W",L"--:--"); // eta_s is time-to-full, never remaining use.
    s.mode=PowerMode::AcIdle; check(s,L"18.2W",L"--:--");
    s.mode=PowerMode::NoBattery; check(s,L"18.2W",L"--:--");
    s.mode=PowerMode::Discharging; s.power_trace=PowerTrace::BatterySide;
    s.battery_supplemental=true; check(s,L"18.2W",L"--:--");
    s.system_w=NAN; s.pkg_w=7.4; check(s,L"--W",L"--:--"); // no package substitution.
    s.system_w=0; check(s,L"0.0W",L"--:--");
    s.system_w=-1; check(s,L"--W",L"--:--");
    s.system_w=1e6; check(s,L"--W",L"--:--");
    s.system_w=18.2; s.power_trace=PowerTrace::Unknown; check(s,L"--W",L"--:--");
    std::cout<<checks<<" compact readout cases passed\n";
}
