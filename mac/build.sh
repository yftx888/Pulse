#!/bin/bash
set -euo pipefail
if ! /usr/bin/xcode-select -p >/dev/null 2>&1; then
    echo '缺少命令行工具，请先运行 xcode-select --install，再重新编译。'
    exit 1
fi
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HERE/build"
FLAGS=(-std=c++17 -fobjc-arc -fblocks -O2 -Wall -Wextra -arch arm64 -mmacosx-version-min=13.0 -I"$HERE/../src")
BACKEND=("$HERE/metrics_mac.mm" "$HERE/netprobe_mac.mm" "$HERE/smc_reader.mm" "$HERE/smc_mac.mm")
FRAMEWORKS=(-framework Foundation -framework IOKit -framework CoreFoundation -framework CFNetwork -framework Network)
/usr/bin/xcrun clang++ "${FLAGS[@]}" "${BACKEND[@]}" "$HERE/metrics_selftest_mac.mm" \
    "${FRAMEWORKS[@]}" \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist \
    -Xlinker "$HERE/metrics_selftest-Info.plist" \
    -o "$HERE/build/metrics_selftest"
echo "已生成 $HERE/build/metrics_selftest"
APP="$HERE/build/Pulse.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
/usr/bin/xcrun clang++ "${FLAGS[@]}" "${BACKEND[@]}" \
    "$HERE/app_main.mm" "$HERE/menubar_mac.mm" "$HERE/renderer_mac.mm" "$HERE/panel_mac.mm" \
    "$HERE/settings_mac.mm" "$HERE/system_language_mac.mm" "$HERE/../src/ui_model.cpp" "$HERE/../src/i18n.cpp" \
    "${FRAMEWORKS[@]}" -framework AppKit -framework ServiceManagement -framework CoreGraphics -framework CoreText -framework QuartzCore \
    -o "$APP/Contents/MacOS/Pulse"
cp "$HERE/Pulse-Info.plist" "$APP/Contents/Info.plist"
if [[ -f "$HERE/AppIcon.icns" ]]; then
    cp "$HERE/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
    /usr/libexec/PlistBuddy -c 'Add :CFBundleIconFile string AppIcon' "$APP/Contents/Info.plist"
fi
/usr/bin/plutil -lint "$APP/Contents/Info.plist"
/usr/bin/codesign --force --sign - "$APP"
/usr/bin/codesign --verify --strict "$APP"
/usr/bin/ditto -c -k --keepParent "$APP" "$HERE/build/Pulse-mac-arm64.zip"
echo "已生成 $APP 与 $HERE/build/Pulse-mac-arm64.zip（ad-hoc 签名，未公证）"
