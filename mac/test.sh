#!/bin/bash
# Offline checks for the fixed whole-machine watts / remaining-time status item.
# Requires the existing Xcode Command Line Tools and a local macOS GUI session.
set -euo pipefail

RUN_RENDER=1
if (( $# > 1 )); then echo "用法：bash mac/test.sh [--logic-only]" >&2; exit 2; fi
case "${1-}" in
    "") ;;
    --logic-only) RUN_RENDER=0 ;;
    *) echo "用法：bash mac/test.sh [--logic-only]" >&2; exit 2 ;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HERE/build"
RESULTS="$(mktemp -d "$HERE/build/gui-tests-XXXXXX")"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -arch arm64 -mmacosx-version-min=13.0 -I"$HERE/../src")
SHARED=("$HERE/../src/ui_model.cpp" "$HERE/../src/i18n.cpp" "$HERE/system_language_mac.mm")

echo "测试记录：$RESULTS"
/usr/bin/xcrun clang++ "${FLAGS[@]}" "$HERE/compact_readouts_test.cpp" "${SHARED[@]}" \
    -framework Foundation -o "$RESULTS/compact_readouts_test" 2>&1 | tee "$RESULTS/readout-build.log"
"$RESULTS/compact_readouts_test" 2>&1 | tee "$RESULTS/readout-tests.txt"
if (( RUN_RENDER == 0 )); then
    echo "纯逻辑检查通过；未运行需要 GUI 会话的绘制检查。"
    exit 0
fi

# This includes the production status view, but never starts Metrics, registers
# a status item or enables network probes. PNGs exercise actual AppKit drawing.
/usr/bin/xcrun clang++ "${FLAGS[@]}" -fobjc-arc -fblocks \
    "$HERE/metrics_mac.mm" "$HERE/netprobe_mac.mm" "$HERE/smc_reader.mm" "$HERE/smc_mac.mm" \
    "$HERE/compact_render_test.mm" "$HERE/renderer_mac.mm" "$HERE/panel_mac.mm" \
    "$HERE/settings_mac.mm" "${SHARED[@]}" \
    -framework Foundation -framework IOKit -framework CoreFoundation -framework CFNetwork \
    -framework Network -framework AppKit -framework ServiceManagement -framework CoreGraphics \
    -framework CoreText -framework QuartzCore -o "$RESULTS/compact_render_test" \
    2>&1 | tee "$RESULTS/render-build.log"
"$RESULTS/compact_render_test" "$RESULTS/render-cases" 2>&1 | tee "$RESULTS/render-tests.txt"
echo "自动检查通过；真实菜单栏、供电切换及交互验收请另行记录。"
