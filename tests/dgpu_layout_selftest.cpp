// DirectWrite layout check; no window, D3D device, or NVML queries.
#include "ui_renderer.h"
#include <cmath>
#include <cstdio>

int main() {
    using namespace pulse;
    using namespace pulse::ui;
    for (const auto language : {i18n::Preference::Chinese, i18n::Preference::English}) {
        i18n::SetLanguage(language);
        Renderer renderer;
        if (FAILED(renderer.Initialize())) {
            std::puts("FAIL: renderer initialization");
            return 1;
        }
        renderer.SetComponents(Components{}, false);
        const float width = renderer.FullWidth();
        const bool chinese = language == i18n::Preference::Chinese;
        std::printf("%s full_width=%.0f DIP (delta from 320=%.0f DIP)\n",
            chinese ? "zh" : "en", width, width - 320);
        if (chinese && std::fabs(width - 320) > .01f) {
            std::puts("FAIL: Chinese default width must remain 320 DIP");
            return 1;
        }
        Model model;
        Snapshot sample;
        sample.gpu_meter = MeterStatus::Valid;
        sample.gpu_w = 5;
        for (const auto status : {DgpuStatus::Sleeping, DgpuStatus::Glitch}) {
            sample.dgpu_status = status;
            model.Update(sample, false, Components{});
            const std::wstring expected = status == DgpuStatus::Sleeping ?
                i18n::T(i18n::Key::DgpuSleeping) : i18n::T(i18n::Key::DgpuPrefix) + L"--";
            if (model.Data().gpuW != expected) {
                std::puts("FAIL: dGPU sleep/glitch label");
                return 1;
            }
        }
        const auto card = FullLayout(Components{}, width).Card(FullComponent::Hardware);
        if (std::fabs(card.bottom - card.top - 96) > .01f) {
            std::puts("FAIL: hardware card height changed");
            return 1;
        }
    }
    std::puts("dgpu_layout_selftest: PASS");
    return 0;
}
