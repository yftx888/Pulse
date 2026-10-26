// Directed checks for the shared power classifier and the UI model.
// Not part of build.bat. See tests/README.md for the g++ command.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "ui_model.h"
#include "power_diagnostics.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

void PrintW(const wchar_t* text) {
    if (!text) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), bytes, nullptr, nullptr);
    std::fputs(utf8.c_str(), stdout);
}

void Expect(bool ok, const char* name) {
    if (ok) {
        std::printf("ok %s\n", name);
        return;
    }
    std::printf("FAIL %s\n", name);
    ++g_fails;
}

void ExpectEq(const std::wstring& actual, const std::wstring& expected, const char* name) {
    if (actual == expected) {
        std::printf("ok %s\n", name);
        return;
    }
    std::printf("FAIL %s\n  expected: ", name);
    PrintW(expected.c_str());
    std::printf("\n  actual:   ");
    PrintW(actual.c_str());
    std::printf("\n");
    ++g_fails;
}

void ExpectHas(const std::wstring& actual, const std::wstring& part, const char* name) {
    Expect(actual.find(part) != std::wstring::npos, name);
    if (actual.find(part) == std::wstring::npos) {
        std::printf("  missing: ");
        PrintW(part.c_str());
        std::printf("\n  actual:  ");
        PrintW(actual.c_str());
        std::printf("\n");
    }
}

void ExpectLacks(const std::wstring& actual, const std::wstring& part, const char* name) {
    Expect(actual.find(part) == std::wstring::npos, name);
    if (actual.find(part) != std::wstring::npos) {
        std::printf("  unexpected: ");
        PrintW(part.c_str());
        std::printf("\n  actual:     ");
        PrintW(actual.c_str());
        std::printf("\n");
    }
}

int NonNegative(const std::vector<float>& history) {
    int count = 0;
    for (float value : history)
        if (value >= 0.0f) ++count;
    return count;
}

pulse::BatterySample Sample() {
    pulse::BatterySample in;
    in.read_ok = true;
    in.present = true;
    return in;
}

void CheckClassifier() {
    using namespace pulse;
    {
        BatterySample in = Sample();
        in.present = false;
        in.ac_known = true;
        in.ac_online = true;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::NoBattery, "no battery mode");
        Expect(out.trace == PowerTrace::Package, "no battery uses package");
        Expect(!out.pure_battery && !out.supplemental && !out.show_watts, "no battery has no battery power");
        Expect(std::isnan(out.rate_mw), "no battery rate stays unknown");
    }
    {
        BatterySample in;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::Unknown && out.trace == PowerTrace::Unknown, "failed read is unknown");
        Expect(!out.ac_known && !out.ac_online && !out.rate_valid, "failed read confirms nothing");
        Expect(std::isnan(out.rate_mw) && !out.show_watts && !out.pure_battery, "failed read is not 0 W");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = -12000;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::Discharging && out.pure_battery, "pure battery discharge");
        Expect(out.trace == PowerTrace::BatteryWhole && out.show_watts && out.eta_discharge, "pure battery ETA");
        Expect(!out.supplemental && !out.contested && out.rate_mw == -12000, "pure battery keeps the signed rate");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.ac_online = true;
        in.charging = true;
        in.rate_known = true;
        in.rate_mw = 8000;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::Charging && out.trace == PowerTrace::Package, "charge uses package");
        Expect(out.show_watts && out.eta_charge && !out.pure_battery && !out.supplemental, "charge ETA");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.ac_online = true;
        in.rate_known = true;
        in.rate_mw = 0;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::AcIdle && out.trace == PowerTrace::Package, "AC idle");
        Expect(!out.show_watts && !out.pure_battery && !out.supplemental, "zero rate is not charge or discharge");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.ac_online = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = -5000;
        const auto out = ClassifyBattery(in);
        Expect(out.supplemental && out.mode == PowerMode::Discharging, "AC plus discharge is supplemental");
        Expect(out.trace == PowerTrace::Package && !out.pure_battery && !out.eta_discharge, "supplemental is not whole-machine");
        Expect(out.show_watts && out.rate_mw == -5000, "supplemental keeps signed discharge");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.discharging = true;
        const auto out = ClassifyBattery(in);
        Expect(out.pure_battery && out.trace == PowerTrace::BatteryWhole, "flag discharge on battery is pure");
        Expect(!out.show_watts && !out.eta_discharge && std::isnan(out.rate_mw), "unknown rate has no watts or derived ETA");
        Expect(UseSystemBatteryEta(out, NAN, 3600), "unknown rate on pure battery keeps system ETA fallback");
        Expect(!UseSystemBatteryEta(out, 1800, 3600), "system ETA does not replace a derived estimate");
        Expect(!UseSystemBatteryEta(out, NAN, NAN) && !UseSystemBatteryEta(out, NAN, INFINITY),
               "system ETA must be finite");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.ac_online = true;
        in.discharging = true;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::AcIdle && !out.supplemental &&
               out.trace == PowerTrace::Package && !out.show_watts, "unknown rate on AC is idle");
        Expect(!UseSystemBatteryEta(out, NAN, 3600), "mixed discharge cannot use system ETA");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = 0;
        const auto out = ClassifyBattery(in);
        Expect(out.mode == PowerMode::AcIdle && !out.pure_battery && !out.show_watts, "zero rate ignores a lone flag");
        Expect(out.trace == PowerTrace::Package, "zero rate is not whole-machine");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.charging = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = -4000;
        const auto out = ClassifyBattery(in);
        Expect(out.contested && out.mode == PowerMode::Discharging && out.show_watts, "contested negative still discharges");
        Expect(!out.pure_battery && !out.eta_discharge && out.trace == PowerTrace::BatterySide, "contested negative is not whole-machine");
        Expect(!UseSystemBatteryEta(out, NAN, 3600), "conflicting flags cannot use system ETA");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.ac_online = true;
        in.charging = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = 4000;
        const auto out = ClassifyBattery(in);
        Expect(out.contested && out.mode != PowerMode::Charging && !out.show_watts, "contested positive is not charging");
        Expect(out.trace == PowerTrace::Package && out.rate_valid, "contested positive keeps the rate metadata");
    }
    {
        BatterySample in = Sample();
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = -2500;
        const auto out = ClassifyBattery(in);
        Expect(!out.ac_known && !out.pure_battery && !out.supplemental, "unknown AC is not whole-machine");
        Expect(out.trace == PowerTrace::BatterySide && out.show_watts, "unknown AC discharge stays on the battery side");
        Expect(!UseSystemBatteryEta(out, NAN, 3600), "unknown AC cannot use system ETA");
    }
    {
        BatterySample in = Sample();
        in.ac_known = true;
        in.ac_online = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = -500;
        const auto idle = ClassifyBattery(in);
        Expect(idle.mode == PowerMode::AcIdle && !idle.supplemental && !idle.show_watts,
               "AC discharge below 1 W is idle");
        Expect(idle.trace == PowerTrace::Package && !idle.eta_discharge && !idle.pure_battery,
               "small AC discharge has no ETA or unplug average");
        Expect(idle.rate_valid && idle.rate_mw == -500, "small discharge keeps raw rate metadata");
        in.rate_mw = -kAcDischargeMinMw;
        Expect(ClassifyBattery(in).supplemental, "AC threshold includes exactly 1 W");
        in.rate_mw = -999.9;
        Expect(!ClassifyBattery(in).supplemental, "AC threshold rejects just below 1 W");
        in.ac_online = false;
        Expect(ClassifyBattery(in).pure_battery, "small offline discharge stays pure battery");
        in.ac_known = false;
        Expect(ClassifyBattery(in).trace == PowerTrace::BatterySide, "small unknown-AC discharge stays battery-side");
    }
    Expect(!NeedsPackagePower(PowerTrace::BatteryWhole) && !NeedsPackagePower(PowerTrace::BatterySide),
           "battery traces can skip package power");
    Expect(NeedsPackagePower(PowerTrace::Package) && NeedsPackagePower(PowerTrace::Unknown),
           "package and unknown still need package power");
    Expect(BatteryRateKnown(0) && BatteryRateKnown(499999) && BatteryRateKnown(-499999), "normal rates are known");
    Expect(!BatteryRateKnown(500000) && !BatteryRateKnown(-500000) && !BatteryRateKnown(static_cast<long>(0x80000000)),
           "unknown-rate sentinels are not known");
}

void CheckAcDischargeDebounce() {
    using pulse::AcDischargeDebounce;
    AcDischargeDebounce debounce;
    Expect(!debounce.Update(true, false, 0), "AC debounce starts at time zero");
    Expect(!debounce.Update(true, false, 9900), "AC debounce unconfirmed at 9.9 s");
    Expect(debounce.Update(true, false, 10000), "AC debounce confirmed at 10 s");
    Expect(debounce.Update(false, false, 11000), "AC debounce starts exit hold");
    Expect(debounce.Update(false, false, 15900), "AC debounce holds for 4.9 s");
    Expect(!debounce.Update(false, false, 16000), "AC debounce exits at 5 s");

    debounce.Reset();
    Expect(!debounce.Update(true, false, 0), "AC debounce starts entry noise sequence");
    Expect(!debounce.Update(false, false, 9900), "AC debounce false resets entry timing");
    Expect(!debounce.Update(true, false, 10000), "AC debounce entry timing restarts");
    Expect(!debounce.Update(true, false, 19900), "AC debounce restarted entry needs 10 s");
    Expect(debounce.Update(true, false, 20000), "AC debounce restarted entry confirms");
    Expect(debounce.Update(false, false, 21000), "AC debounce starts exit noise sequence");
    Expect(debounce.Update(false, false, 25900), "AC debounce exit noise holds at 4.9 s");
    Expect(debounce.Update(true, false, 25950), "AC debounce true cancels pending exit");
    Expect(debounce.Update(false, false, 26000), "AC debounce exit timing restarts");
    Expect(debounce.Update(false, false, 30900), "AC debounce restarted exit needs 5 s");
    Expect(!debounce.Update(false, false, 31000), "AC debounce restarted exit completes");

    debounce.Reset();
    Expect(!debounce.Update(true, false, 0), "AC debounce starts hard-exit entry");
    Expect(!debounce.Update(true, true, 9000), "AC debounce hard exit clears pending entry");
    Expect(!debounce.Update(true, false, 10000), "AC debounce hard exit discards old entry time");
    Expect(debounce.Update(true, false, 20000), "AC debounce confirms after fresh 10 s");
    Expect(!debounce.Update(true, true, 20001), "AC debounce hard exit overrides raw true immediately");
    Expect(!debounce.Update(true, false, 20002), "AC debounce confirmed hard exit starts fresh");
    Expect(!debounce.Update(true, false, 29902), "AC debounce no stale confirmed state");
    Expect(debounce.Update(true, false, 30002), "AC debounce can confirm again");
    debounce.Reset();
    Expect(!debounce.Update(false, false, 30003), "AC debounce explicit reset clears confirmed state");

    AcDischargeDebounce mixed, half_second;
    const unsigned long long epoch = 1ULL << 40;
    for (unsigned long long elapsed = 0; elapsed <= 10000; elapsed += 500)
        Expect(half_second.Update(true, false, epoch + elapsed) == (elapsed >= 10000),
               "AC debounce 0.5 s entry follows elapsed time");
    for (unsigned long long elapsed : {0ULL, 500ULL, 2500ULL, 3000ULL, 5000ULL,
                                       5500ULL, 7500ULL, 8000ULL, 10000ULL})
        Expect(mixed.Update(true, false, epoch + elapsed) == (elapsed >= 10000),
               "AC debounce mixed 0.5 s and 2 s entry matches fixed sampling");
    for (unsigned long long elapsed = 0; elapsed <= 5000; elapsed += 500)
        Expect(half_second.Update(false, false, epoch + 10000 + elapsed) == (elapsed < 5000),
               "AC debounce 0.5 s exit follows elapsed time");
    for (unsigned long long elapsed : {0ULL, 500ULL, 2500ULL, 3000ULL, 5000ULL})
        Expect(mixed.Update(false, false, epoch + 10000 + elapsed) == (elapsed < 5000),
               "AC debounce mixed 0.5 s and 2 s exit matches fixed sampling");
}

void CheckMeterInterpretation() {
    using namespace pulse;
    const MeterEvidence absent{}, empty{true, true, false}, failed{true, false, false}, matched{true, true, true};
    // These functions are called by the actual sampler and manual diagnostic.
    Expect(ResolveMeterStatus(absent, absent, NAN, false) == MeterStatus::CounterUnavailable, "no counters");
    Expect(ResolveMeterStatus(empty, empty, NAN, false) == MeterStatus::NoMatch, "successful arrays without a domain");
    Expect(ResolveMeterStatus(failed, empty, NAN, false) == MeterStatus::ReadFailed, "Energy array failure is not no-match");
    Expect(ResolveMeterStatus(empty, failed, NAN, false) == MeterStatus::ReadFailed, "Power array failure is not no-match");
    Expect(ResolveMeterStatus(matched, matched, NAN, false) == MeterStatus::ReadFailed, "matched invalid rows are not no-match");
    Expect(ResolveMeterStatus(matched, failed, NAN, true) == MeterStatus::Warmup, "valid Energy baseline is warming");
    const std::uint32_t bad = 0xC0000BBAu;
    const auto powerOnly = InterpretMeterDomain(absent, matched, nullptr, nullptr, 1, 0, 5500.25);
    Expect(powerOnly.status == MeterStatus::Valid && powerOnly.power_w == 5.50025 && std::isnan(powerOnly.energy_w),
           "Energy unavailable, Power-only fallback preserves fractional mW");
    const auto energyFail = InterpretMeterDomain(failed, matched, nullptr, nullptr, 1, 1, 7000);
    Expect(energyFail.status == MeterStatus::Valid && energyFail.power_w == 7, "Energy array failed, Power still usable");
    const auto noRows = InterpretMeterDomain(failed, empty, nullptr, nullptr, 1, bad, NAN);
    Expect(noRows.status == MeterStatus::ReadFailed, "array failed is not an empty provider");
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0, 1.0e9}) {
        const auto result = InterpretMeterDomain(absent, matched, nullptr, nullptr, 1, 0, invalid);
        Expect(result.status == MeterStatus::ReadFailed && std::isnan(result.power_w), "invalid Power value is not watts");
    }
    const auto badPower = InterpretMeterDomain(absent, matched, nullptr, nullptr, 1, bad, 5000);
    Expect(badPower.status == MeterStatus::ReadFailed && std::isnan(badPower.power_w), "bad Power CStatus is not watts");
    const auto zeroPower = InterpretMeterDomain(absent, matched, nullptr, nullptr, 1, 0, 0);
    Expect(zeroPower.status == MeterStatus::Valid && zeroPower.power_w == 0, "valid zero Power is allowed");

    MeterEnergySample before{L"package0_pkg", 0, 9007199254740992ULL};
    MeterEnergySample after{before.name, 1, before.pwh + 1000000000ULL};
    auto result = InterpretMeterDomain(matched, absent, &before, &after, 2, bad, NAN);
    Expect(result.status == MeterStatus::Valid && std::fabs(result.energy_w - 1.8) < 1e-12 &&
           result.delta_pwh == 1000000000ULL, "valid same-name integer Energy delta uses pWh and QPC seconds");
    before.status = bad;
    result = InterpretMeterDomain(matched, absent, &before, &after, 2, bad, NAN);
    Expect(result.status == MeterStatus::ReadFailed && std::isnan(result.energy_w), "bad first CStatus forbids Energy subtraction");
    before.status = 0;
    after.status = bad;
    result = InterpretMeterDomain(matched, matched, &before, &after, 2, 0, 3300);
    Expect(result.status == MeterStatus::Valid && std::isnan(result.energy_w) && result.power_w == 3.3,
           "bad second CStatus can use valid Power fallback");
    result = InterpretMeterDomain(matched, absent, &before, &after, 2, bad, NAN);
    Expect(result.status == MeterStatus::ReadFailed && result.interval == MeterInterval::InvalidData,
           "bad second CStatus is a read failure");
    after.status = 0;
    after.pwh = before.pwh;
    result = InterpretMeterDomain(matched, absent, &before, &after, 1, bad, NAN);
    Expect(result.status == MeterStatus::Warmup && result.interval == MeterInterval::Unchanged && std::isnan(result.energy_w),
           "unchanged diagnostic Energy is not false 0 W");
    after.pwh = before.pwh - 1;
    result = InterpretMeterDomain(matched, absent, &before, &after, 1, bad, NAN);
    Expect(result.status == MeterStatus::Warmup && result.interval == MeterInterval::Reset, "counter reset needs a new baseline");
    after.name = L"other_package0_pkg";
    result = InterpretMeterDomain(matched, absent, &before, &after, 1, bad, NAN);
    Expect(result.status == MeterStatus::Warmup && std::isnan(result.energy_w), "different Energy identities are never subtracted");
    result = InterpretMeterDomain(matched, absent, nullptr, &after, 1, bad, NAN);
    Expect(result.status == MeterStatus::Warmup, "first Energy sample is warmup");
}

pulse::ui::Components FullView() {
    pulse::ui::Components components;
    components.full[static_cast<std::size_t>(pulse::ui::FullComponent::PowerChart)] = true;
    return components;
}

pulse::Snapshot Base() {
    pulse::Snapshot s;
    s.percent = 80;
    s.ac_known = true;
    s.ac_online = true;
    s.active_plan_valid = true;
    s.active_plan_balanced = true;
    s.active_plan_name = L"平衡";
    return s;
}

void CheckModel() {
    using namespace pulse;
    using namespace pulse::ui;
    pulse::i18n::SetLanguage(pulse::i18n::Preference::Chinese);
    const Components full = FullView();
    Expect(FullComponents[static_cast<std::size_t>(FullComponent::Hardware)].extent == 96,
           "hardware card keeps the HEAD height");
    ExpectEq(T(Key::AssistChip), L"插电放电", "Chinese neutral AC discharge label");
    {
        Model model;
        Snapshot s = Base();
        s.active_plan_name = L"我的夜间计划";
        s.active_plan_balanced = false;
        s.mode = PowerMode::AcIdle;
        s.battery_rate_valid = true;
        s.battery_rate_mw = 0;
        s.power_trace = PowerTrace::Package;
        s.pkg_w = 4;
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectHas(v.scheme, L"电源计划 · ", "Chinese plan prefix");
        ExpectHas(v.scheme, L"我的夜间计划", "custom plan is not translated");
        ExpectEq(v.saverLabel, L"", "saver hidden while off");
        ExpectEq(v.chip, T(Key::OnAc), "AC idle chip");
        ExpectLacks(v.foot, L"拔电", "AC idle has no unplug average");
    }
    {
        Model model;
        Snapshot s = Base();
        s.active_plan_name = L"平衡";
        s.mode = PowerMode::AcIdle;
        s.power_trace = PowerTrace::Package;
        model.Update(s, false, full, false);
        s.active_plan_valid = false;
        s.active_plan_name = L"平衡";
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectEq(v.scheme, T(Key::PowerPlanPrefix) + T(Key::PowerPlanUnknown), "failed plan query drops the old name");
        ExpectLacks(v.scheme, L"平衡", "stale plan name is not displayed");
    }
    {
        Model model;
        Snapshot s = Base();
        s.battery_saver = true;
        s.active_plan_name = L"卓越性能";
        s.active_plan_balanced = false;
        s.mode = PowerMode::AcIdle;
        s.power_trace = PowerTrace::Package;
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectHas(v.scheme, L"卓越性能", "saver keeps the real plan");
        ExpectEq(v.saverLabel, T(Key::BatterySaverShort), "saver is a short tag");
        ExpectLacks(v.scheme, T(Key::BatterySaver), "long saver text does not replace the plan");
    }
    {
        Model model;
        Snapshot s = Base();
        s.active_plan_name = std::wstring(40, L'计');
        s.active_plan_balanced = false;
        s.mode = PowerMode::AcIdle;
        s.power_trace = PowerTrace::Package;
        model.Update(s, false, full, false);
        ExpectEq(model.Data().scheme, T(Key::PowerPlanPrefix) + s.active_plan_name, "long plan stays complete in the model");
    }
    {
        Model model;
        Snapshot s = Base();
        s.mode = PowerMode::Discharging;
        s.battery_supplemental = true;
        s.power_contested = false;
        s.battery_rate_valid = true;
        s.battery_rate_mw = -12000;
        s.power_w = 12;
        s.pkg_w = 7;
        s.power_trace = PowerTrace::Package;
        s.eta_s = 5000;
        s.eta20_s = 4000;
        s.on_battery_s = 100;
        s.avg_w = 9;
        s.pct_per_hour = 8;
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectEq(v.powerLabel, T(Key::Processor), "mixed power column stays on the processor");
        ExpectEq(v.powerValue, L"7.0", "mixed power column uses package watts");
        Expect(v.powerLabel != T(Key::System), "mixed power is not whole-machine");
        ExpectEq(v.heroWord, T(Key::ExternalDischarge), "mixed hero");
        ExpectEq(v.chip, T(Key::AssistChip), "mixed chip");
        ExpectEq(v.wattLabel, T(Key::NetDischarge), "mixed net-discharge label");
        ExpectEq(v.watt, L"12.0", "mixed battery card shows the battery watts");
        ExpectEq(v.pph, L"--", "mixed has no per-hour ETA");
        ExpectEq(v.eta20, L"", "mixed has no to-20%");
        ExpectLacks(v.foot, L"拔电", "mixed does not show the unplug average");
        ExpectLacks(v.etaLabel, T(Key::EtaLeft), "mixed does not show time left");
    }
    {
        Model model;
        Snapshot s = Base();
        s.ac_online = false;
        s.mode = PowerMode::Discharging;
        s.battery_supplemental = false;
        s.battery_rate_valid = true;
        s.battery_rate_mw = -11000;
        s.power_w = 11;
        s.pkg_w = 99;
        s.power_trace = PowerTrace::BatteryWhole;
        s.eta_s = 3600;
        s.eta20_s = 1800;
        s.on_battery_s = 120;
        s.avg_w = 10;
        s.percent = 50;
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectEq(v.powerLabel, T(Key::System), "pure battery power is whole-machine");
        ExpectEq(v.powerValue, L"11.0", "pure battery uses battery watts");
        Expect(!v.hero.empty(), "pure battery shows an ETA");
        ExpectHas(v.foot, T(Key::SinceUnplugged), "pure battery shows the unplug interval");
        ExpectHas(v.etaLabel, T(Key::EtaLeft), "pure battery time-left label");
        Expect(v.eta20 != L"", "pure battery shows to-20%");
    }
    {
        Model model;
        Snapshot mixed = Base();
        mixed.mode = PowerMode::Discharging;
        mixed.battery_supplemental = true;
        mixed.battery_rate_valid = true;
        mixed.power_w = 12;
        mixed.pkg_w = 10;
        mixed.power_trace = PowerTrace::Package;
        model.Update(mixed, false, full, false);
        model.Update(mixed, false, full, false);
        Expect(NonNegative(model.Data().powerHistory) == 2, "same package source keeps chart history");
        Snapshot gap = mixed;
        gap.power_trace = PowerTrace::Unknown;
        gap.ac_known = false;
        gap.battery_rate_valid = false;
        gap.power_w = 0;
        gap.pkg_w = 0;
        model.Update(gap, false, full, false);
        Expect(NonNegative(model.Data().powerHistory) == 2, "one failed read does not append or clear history");
        ExpectEq(model.Data().powerValue, L"--", "failed read does not draw 0 W");
        Snapshot pure = Base();
        pure.ac_online = false;
        pure.mode = PowerMode::Discharging;
        pure.battery_rate_valid = true;
        pure.power_w = 30;
        pure.pkg_w = 10;
        pure.power_trace = PowerTrace::BatteryWhole;
        pure.eta_s = 3600;
        model.Update(pure, false, full, false);
        Expect(NonNegative(model.Data().powerHistory) == 1, "source change breaks the chart");
        Expect(model.Data().powerHistory.back() == 30.0f, "new chart point is battery power");
        ExpectEq(model.Data().powerLabel, T(Key::System), "chart returns to whole-machine");
    }
    {
        Model model;
        Snapshot s = Base();
        s.ac_known = false;
        s.ac_online = false;
        s.mode = PowerMode::Discharging;
        s.battery_rate_valid = true;
        s.power_w = 4;
        s.pkg_w = 8;
        s.power_trace = PowerTrace::BatterySide;
        s.on_battery_s = 50;
        s.avg_w = 4;
        model.Update(s, false, full, false);
        ExpectEq(model.Data().powerLabel, T(Key::Battery), "unknown AC is battery, not whole-machine");
        ExpectLacks(model.Data().foot, L"拔电", "unknown AC has no unplug average");
        ExpectEq(model.Data().watt, L"4.0", "battery-side watts stay visible");
    }
    {
        Model model;
        Snapshot s = Base();
        BatterySample in = Sample();
        in.ac_known = true;
        in.discharging = true;
        in.rate_known = true;
        in.rate_mw = 0;
        const auto classified = ClassifyBattery(in);
        s.ac_known = classified.ac_known;
        s.ac_online = classified.ac_online;
        s.mode = classified.mode;
        s.battery_rate_valid = classified.rate_valid;
        s.battery_rate_mw = classified.rate_mw;
        s.power_w = classified.show_watts ? std::fabs(classified.rate_mw) / 1000.0 : NAN;
        s.power_trace = classified.trace;
        s.pkg_w = 3;
        model.Update(s, false, full, false);
        ExpectEq(model.Data().powerLabel, T(Key::Processor), "zero rate on battery is not whole-machine");
        ExpectEq(model.Data().heroWord, T(Key::NotCharging), "offline idle is not external power");
        ExpectEq(model.Data().watt, L"--", "zero rate is not shown as watts");
        Expect(!model.Data().acOnline && !model.Data().supplemental, "offline zero rate cannot draw a plugged-in icon");
        model.Update(s, true, full, false);
        ExpectEq(model.Data().compactEtaValue, T(Key::NotCharging), "offline zero rate compact is not fully charged");
        Expect(!model.Data().acOnline && model.Data().mode != PowerMode::Charging, "offline zero rate compact has no AC/charge icon");
    }
    {
        Model model;
        Snapshot s = Base();
        s.pkg_meter = MeterStatus::NoMatch;
        s.pkg_w = 0;
        s.cpu_meter = MeterStatus::Warmup;
        s.cpu_w = 0;
        s.gpu_meter = MeterStatus::CounterUnavailable;
        s.gpu_w = 0;
        s.mem_meter = MeterStatus::Off;
        s.mem_w = 0;
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectEq(v.packageW, L"--", "missing package is not 0 W");
        ExpectEq(v.packageNote, T(Key::MeterNoDomain), "missing package reason");
        ExpectEq(v.cpuW, T(Key::MeterWarmup), "warmup is not a permanent absence");
        ExpectEq(v.gpuW, T(Key::MeterNoCounter), "unavailable counter reason");
        ExpectLacks(v.gpuW, L"0", "unavailable GPU power is not 0");
        ExpectEq(v.memW, L"--", "disabled memory meter is unknown");
    }
    {
        Model model;
        Snapshot s = Base();
        s.gpu_meter = MeterStatus::Valid;
        s.gpu_w = 5.5;
        s.pkg_meter = MeterStatus::Valid;
        s.pkg_w = 9;
        model.Update(s, false, full, false);
        ExpectEq(model.Data().gpuW, L"5.5" + T(Key::WattSuffix), "GPU numeric line stays compact");
        ExpectEq(model.Data().packageNote, T(Key::WholeChip), "valid package note");
    }
    {
        Components components;
        components.full[static_cast<std::size_t>(FullComponent::Hardware)] = false;
        components.full[static_cast<std::size_t>(FullComponent::PowerChart)] = true;
        const Wants battery = CollectionWants(components, DisplayMode::Full, false, PowerTrace::BatteryWhole);
        const Wants package = CollectionWants(components, DisplayMode::Full, false, PowerTrace::Package);
        const Wants unknown = CollectionWants(components, DisplayMode::Full, false, PowerTrace::Unknown);
        Expect(!battery.chip_power, "pure battery chart can skip package power");
        Expect(package.chip_power && unknown.chip_power, "AC or unknown chart still collects package power");
        Components compact;
        const Wants compactBattery = CollectionWants(compact, DisplayMode::Compact, false, PowerTrace::BatteryWhole);
        const Wants compactPackage = CollectionWants(compact, DisplayMode::Compact, false, PowerTrace::Package);
        Expect(!compactBattery.chip_power && compactPackage.chip_power, "compact follows the same source rule");
        Components taskbar;
        const Wants stripBattery = CollectionWants(taskbar, DisplayMode::Taskbar, false, PowerTrace::BatteryWhole);
        const Wants stripPackage = CollectionWants(taskbar, DisplayMode::Taskbar, false, PowerTrace::Package);
        Expect(!stripBattery.chip_power && stripPackage.chip_power, "taskbar strip follows the source rule");
        const Wants details = CollectionWants(taskbar, DisplayMode::Taskbar, true, PowerTrace::BatteryWhole);
        Expect(details.chip_power, "taskbar details keep package power while hardware is shown");
    }
    {
        pulse::i18n::SetLanguage(pulse::i18n::Preference::English);
        Components selection;
        selection.compact = selection.taskbar;
        Model model;
        Snapshot s = Base();
        s.mode = PowerMode::Discharging;
        s.battery_supplemental = true;
        s.battery_rate_valid = true;
        s.power_w = 6;
        s.pkg_w = 5;
        s.power_trace = PowerTrace::Package;
        s.eta_s = 5000;
        s.on_battery_s = 80;
        model.Update(s, true, selection, true);
        const View& v = model.Data();
        ExpectEq(v.powerLabel, T(Key::CompactProcessor), "taskbar mixed power stays on the processor");
        Expect(v.powerLabel != T(Key::Processor), "taskbar uses the compact processor label");
        ExpectEq(v.powerValue, L"5.0", "taskbar mixed power uses package watts");
        Expect(v.powerLabel != T(Key::System), "taskbar mixed power is not whole-machine");
        ExpectEq(v.compactEtaValue, T(Key::AssistChip), "taskbar mixed ETA is the AC discharge label");
        ExpectEq(v.compactEtaLabel, L"", "taskbar mixed ETA has no time-left caption");
    }

    pulse::i18n::SetLanguage(pulse::i18n::Preference::English);
    {
        Model model;
        Snapshot s = Base();
        s.active_plan_name = L"Night Owls";
        s.active_plan_balanced = false;
        s.battery_saver = true;
        s.mode = PowerMode::Discharging;
        s.battery_supplemental = true;
        s.battery_rate_valid = true;
        s.power_w = 12;
        s.pkg_w = 7;
        s.power_trace = PowerTrace::Package;
        s.eta_s = 5000;
        s.on_battery_s = 100;
        s.avg_w = 9;
        model.Update(s, false, full, false);
        const View& v = model.Data();
        ExpectHas(v.scheme, L"Power plan · Night Owls", "English keeps the custom plan");
        ExpectEq(v.saverLabel, L"Saver", "English saver tag");
        ExpectEq(v.powerLabel, L"Processor", "English mixed power label");
        ExpectEq(v.heroWord, L"External power · Battery discharging", "English mixed hero");
        ExpectEq(v.wattLabel, L"Net discharge", "English net discharge");
        ExpectEq(v.chip, L"AC + battery", "English neutral AC discharge label");
        ExpectLacks(v.foot, L"Unplugged", "English mixed has no unplug text");
        s.active_plan_valid = false;
        model.Update(s, false, full, false);
        ExpectEq(model.Data().scheme, L"Power plan · Unknown", "English failed plan query");
        ExpectLacks(model.Data().scheme, L"Night Owls", "English drops the stale plan");
    }
}

void CheckSchemeModel() {
    using namespace pulse;
    using namespace pulse::ui;
    for (const auto language : {i18n::Preference::Chinese, i18n::Preference::English}) {
        i18n::SetLanguage(language);
        const bool chinese = language == i18n::Preference::Chinese;
        Components components;
        Model model;
        Snapshot s = Base();
        s.mode = PowerMode::AcIdle;
        s.active_plan_name = chinese ? L"平衡" : L"Balanced";
        for (const auto kind : {PowerSchemeKind::BestPerformance, PowerSchemeKind::BestEfficiency,
                                PowerSchemeKind::LongerBattery}) {
            s.power_scheme_kind = kind;
            model.Update(s, false, components);
            const Key key = kind == PowerSchemeKind::BestPerformance ? Key::BestPerformance :
                kind == PowerSchemeKind::BestEfficiency ? Key::BestEfficiency : Key::LongerBattery;
            ExpectEq(model.Data().scheme, T(Key::PowerModePrefix) + T(key),
                     "balanced plan shows the non-balanced overlay in both languages");
            if (kind == PowerSchemeKind::BestPerformance)
                ExpectEq(model.Data().scheme, chinese ? L"电源模式 · 最佳性能" : L"Power mode · Best performance",
                         "best-performance overlay uses the exact localized wording");
        }
        for (const auto kind : {PowerSchemeKind::Balanced, PowerSchemeKind::Unknown}) {
            s.power_scheme_kind = kind;
            model.Update(s, false, components);
            ExpectEq(model.Data().scheme, chinese ? L"电源计划 · 平衡" : L"Power plan · Balanced",
                     "balanced or unknown overlay shows the balanced plan name");
        }
        s.active_plan_balanced = false;
        s.active_plan_name = chinese ? L"我的夜间计划" : L"Night Owls";
        for (const auto kind : {PowerSchemeKind::Unknown, PowerSchemeKind::Balanced,
                                PowerSchemeKind::BestPerformance, PowerSchemeKind::BestEfficiency,
                                PowerSchemeKind::LongerBattery}) {
            s.power_scheme_kind = kind;
            model.Update(s, false, components);
            ExpectEq(model.Data().scheme, T(Key::PowerPlanPrefix) + s.active_plan_name,
                     "non-balanced plan wins over every overlay");
        }
        // A custom plan called Balanced must not impersonate the system GUID.
        s.active_plan_name = chinese ? L"平衡" : L"Balanced";
        s.power_scheme_kind = PowerSchemeKind::BestPerformance;
        model.Update(s, false, components);
        ExpectEq(model.Data().scheme, T(Key::PowerPlanPrefix) + s.active_plan_name,
                 "balanced display name alone does not enable overlay");
        s.active_plan_valid = false;
        s.active_plan_balanced = true;
        model.Update(s, false, components);
        ExpectEq(model.Data().scheme, chinese ? L"电源计划 · 未知" : L"Power plan · Unknown",
                 "failed plan ignores stale balanced metadata and overlay");
        s.active_plan_valid = true;
        s.active_plan_name.clear();
        s.power_scheme_kind = PowerSchemeKind::Unknown;
        model.Update(s, false, components);
        ExpectEq(model.Data().scheme, T(Key::PowerPlanPrefix) + T(Key::PowerPlanUnknown),
                 "empty plan name is unknown");
        s.active_plan_name = chinese ? L"平衡" : L"Balanced";
        s.power_scheme_kind = PowerSchemeKind::BestPerformance;
        s.battery_saver = true;
        model.Update(s, false, components);
        ExpectEq(model.Data().saverLabel, T(Key::BatterySaverShort), "saver tag remains alongside overlay");
        ExpectEq(model.Data().scheme, T(Key::PowerModePrefix) + T(Key::BestPerformance),
                 "saver does not replace the overlay");
    }
}

void CheckSystemEtaModel() {
    using namespace pulse;
    using namespace pulse::ui;
    for (const auto language : {i18n::Preference::Chinese, i18n::Preference::English}) {
        i18n::SetLanguage(language);
        BatterySample in = Sample();
        in.ac_known = true;
        in.discharging = true;
        const auto classified = ClassifyBattery(in);
        Snapshot s = Base();
        s.mode = classified.mode;
        s.ac_known = classified.ac_known;
        s.ac_online = classified.ac_online;
        s.power_trace = classified.trace;
        s.battery_rate_valid = classified.rate_valid;
        s.battery_rate_mw = classified.rate_mw;
        s.eta_from_system = UseSystemBatteryEta(classified, NAN, 3600);
        s.eta_s = s.eta_from_system ? 3600 : NAN;
        s.power_w = NAN;
        Components components = FullView();
        Model full;
        full.Update(s, false, components, false);
        Expect(!full.Data().hero.empty(), "unknown rate shows Windows ETA in full view");
        ExpectHas(full.Data().etaLabel, T(Key::SystemEstimate), "Windows ETA is labeled as a system estimate");
        ExpectEq(full.Data().watt, L"--", "system ETA does not invent battery watts");
        ExpectEq(full.Data().powerValue, L"--", "system ETA does not invent chart watts");
        ExpectEq(full.Data().pph, L"--", "system ETA does not invent a drain rate");
        Expect(full.Data().eta20.empty(), "system ETA does not invent to-20%");
        s.eta_from_system = false;
        full.Update(s, false, components, false);
        Expect(full.Data().hero.empty(), "unknown rate without a system estimate hides ETA");
        s.eta_from_system = true;
        for (bool taskbar : {false, true}) {
            Components selection;
            if (taskbar) selection.compact = selection.taskbar;
            Model model;
            model.Update(s, true, selection, taskbar);
            Expect(model.Data().compactEtaFromSystem && model.Data().compactEtaValue != L"--",
                   "compact/taskbar shows flagged Windows ETA");
            s.ac_online = true;
            s.power_trace = PowerTrace::Package;
            s.battery_supplemental = true;
            model.Update(s, true, selection, taskbar);
            ExpectEq(model.Data().compactEtaValue, T(Key::AssistChip), "mixed compact/taskbar rejects stale system ETA");
            Expect(!model.Data().compactEtaFromSystem, "mixed compact/taskbar clears the system ETA flag");
            full.Update(s, false, components, false);
            Expect(full.Data().hero.empty() && full.Data().eta20.empty(), "mixed full view rejects stale system ETA");
            s.ac_online = false;
            s.power_trace = PowerTrace::BatteryWhole;
            s.battery_supplemental = false;
        }
    }
}

void CheckCollectionTrace() {
    using namespace pulse;
    using namespace pulse::ui;
    const Components components;
    const auto opens_package = [&](PowerTrace trace) {
        return CollectionWants(components, DisplayMode::Compact, false, trace).chip_power;
    };

    PowerTrace collection = PowerTrace::Unknown;
    Expect(collection == PowerTrace::Unknown && NeedsPackagePower(collection) && opens_package(collection),
           "initial unknown still needs package power");
    collection = NextCollectionTrace(collection, PowerTrace::Unknown);
    Expect(collection == PowerTrace::Unknown && opens_package(collection),
           "repeated unknown keeps the initial package collection");

    Snapshot observed;
    observed.mode = PowerMode::Unknown;
    observed.power_trace = PowerTrace::Unknown;
    observed.ac_known = false;
    observed.ac_online = false;
    observed.battery_rate_valid = false;
    observed.battery_rate_mw = NAN;
    observed.power_w = NAN;
    const PowerTrace held = NextCollectionTrace(PowerTrace::BatteryWhole, observed.power_trace);
    Expect(held == PowerTrace::BatteryWhole && !opens_package(held),
           "helper return is only the collection cache");
    Expect(observed.power_trace == PowerTrace::Unknown && observed.mode == PowerMode::Unknown &&
               !observed.ac_known && !observed.ac_online && !observed.battery_rate_valid &&
               std::isnan(observed.battery_rate_mw) && std::isnan(observed.power_w),
           "observed unknown snapshot is not rewritten");

    collection = PowerTrace::BatteryWhole;
    collection = NextCollectionTrace(collection, PowerTrace::Unknown);
    Expect(collection == PowerTrace::BatteryWhole && !opens_package(collection),
           "whole to unknown does not open package power");
    collection = NextCollectionTrace(collection, PowerTrace::BatteryWhole);
    Expect(collection == PowerTrace::BatteryWhole && !opens_package(collection),
           "whole to unknown to whole stays off package power");

    collection = PowerTrace::BatteryWhole;
    collection = NextCollectionTrace(collection, PowerTrace::Unknown);
    collection = NextCollectionTrace(collection, PowerTrace::Package);
    Expect(collection == PowerTrace::Package && opens_package(collection),
           "whole to unknown to package restores package power");

    collection = NextCollectionTrace(PowerTrace::Package, PowerTrace::Unknown);
    Expect(collection == PowerTrace::Package && opens_package(collection),
           "package to unknown keeps package power");
    collection = NextCollectionTrace(collection, PowerTrace::Package);
    Expect(collection == PowerTrace::Package && opens_package(collection),
           "package to unknown to package keeps package power");

    collection = NextCollectionTrace(PowerTrace::BatterySide, PowerTrace::Unknown);
    Expect(collection == PowerTrace::BatterySide && !opens_package(collection),
           "side to unknown does not open package power");
    Expect(NextCollectionTrace(PowerTrace::Unknown, PowerTrace::BatterySide) == PowerTrace::BatterySide &&
               !opens_package(PowerTrace::BatterySide),
           "first confirmed side releases package power");
    Expect(NextCollectionTrace(PowerTrace::BatterySide, PowerTrace::Package) == PowerTrace::Package &&
               opens_package(PowerTrace::Package),
           "confirmed hybrid package is collected");
    Expect(NextCollectionTrace(PowerTrace::Package, PowerTrace::BatteryWhole) == PowerTrace::BatteryWhole &&
               !opens_package(PowerTrace::BatteryWhole),
           "confirmed pure battery releases package power");
}

}  // namespace

int main() {
    CheckClassifier();
    CheckAcDischargeDebounce();
    CheckMeterInterpretation();
    CheckModel();
    CheckSchemeModel();
    CheckSystemEtaModel();
    CheckCollectionTrace();
    if (g_fails) {
        std::printf("FAILED %d\n", g_fails);
        return 1;
    }
    std::printf("feedback_selftest: PASS\n");
    return 0;
}
