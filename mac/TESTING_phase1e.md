# macOS 阶段 1e：离线 GUI 验收

代码与阶段 1d 相同（`pulse_mac_phase1d.zip`），只换了这份测试说明。1d 已在 Apple M5 / macOS 26.6 上编译通过：0 错误、0 警告，codesign strict 通过。三种供电状态的离线 selftest 也已完成。本轮补上 GUI。

**本轮全程不联网。** Pulse.app 一律加 `--no-probe`：网络探测始终关闭，程序不发任何网络请求，因此不需要网络授权。不要加 `--require-proxy`，不要跑 `--net`，不要开关 Wi-Fi，也不要改系统设置。输出可能含电池序列号，只交给开发者。

可视交互交给用户亲自验收，见第 5 节，不要用电脑操控工具去点菜单栏。这些交互包括：点击状态项、右键菜单、悬停、真实断网卡片、睡眠、登录项。

## 1. 编译

可以直接用上一轮 1d 的构建产物（`work/mac/build/`），前提是源码未变：先核对 `mac/` 与 `src/` 的哈希和 1d 输入一致。否则在项目根目录执行：

    stamp=$(date +%Y%m%d-%H%M%S)
    backup="mac-validation-$stamp"
    mkdir -p "$backup"
    set -o pipefail
    bash mac/build.sh 2>&1 | tee "$backup/build.log"
    codesign --verify --strict mac/build/Pulse.app

## 2. GUI 快照（离线）

做四组，各一次：纯电池中文、纯电池英文、插电中文、插电英文。每组用新的独立家目录，目录用绝对路径。插电和拔电前后各记一次 `pmset -g batt`。

    home=$(mktemp -d)
    support="$home/Library/Application Support/Pulse"
    mkdir -p "$support"
    printf '[Window]\nlanguage=zh\n' > "$support/settings.ini"
    dir="$backup/snap-battery-zh"
    mkdir -p "$dir"
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse \
        --no-probe --snapshot-dir "$dir" --snapshot-exit 2> "$dir/stderr.txt"
    echo "exit=$?" > "$dir/exit.txt"

- 英文组把 `language=zh` 换成 `language=en`。
- 四组的目录分别为 `snap-battery-zh`、`snap-battery-en`、`snap-ac-zh`、`snap-ac-en`。
- 直接运行二进制，不要用 `open`：这样会继承 `CFFIXED_USER_HOME`，终端会等进程退出。

进程约 6–8 秒后自己退出。每个目录应有 7 个文件：
- `menubar-dark.png`、`menubar-light.png`：菜单栏两行文字可读。
- `details.png`：整张详情卡片。
- `toast-offline.png`、`toast-recovered.png`：程序在屏幕外画的合成卡片，有对应语言的文字，不是空白。
- `menu.txt`、`state.txt`。

`stderr.txt` 里出现 `Pulse:` 开头的行，说明有文件写失败，原文保留，这一项记未通过。

`state.txt` 核对以下字段：

| 字段 | 期望 |
|---|---|
| `language` | `zh` 或 `en` |
| `refresh_ms` | 2000 |
| `no_probe` | yes |
| `probe_enabled` | no |
| `require_proxy` | no |
| `battery_power_source` | `smc-b0ap` |
| `system_power_source` | 纯电池为空，插电为 `smc` |
| `power_trace` | 纯电池 `BatteryWhole`，插电 `Package` |

另外记下 `power_w`、`system_w`、`battery_rate_mw`、`menu_samples`、`elapsed_s`、`cpu_time_s` 的值。菜单栏的延迟列会显示“检测中/Checking”，这是 `--no-probe` 的预期结果，不算失败。

**请对每张图写一两句目测描述**，开发者在 Windows 上看不到 Mac 的渲染。重点是：
- 文字是否被截断或重叠；
- 颜色：充电绿、放电黄、上传紫、下载蓝；
- 浅色背景下是否看得清；
- 详情卡片有没有空白卡或“--”过多。

## 3. GUI 自身耗电（离线）

- 每次用新的家目录，**不要**预写 `settings.ini`，这样刷新间隔保持默认 2 秒，组件也是默认。
- 同一时间只留一个 Pulse。
- 不加 `--snapshot-dir`。

    home=$(mktemp -d)
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse --no-probe &
    pid=$!
    sleep 5
    date +%s > "$backup/cpu-battery-t0.txt"
    ps -o cputime=,rss= -p "$pid" > "$backup/cpu-battery-ps0.txt"
    sleep 300
    date +%s > "$backup/cpu-battery-t1.txt"
    ps -o cputime=,rss= -p "$pid" > "$backup/cpu-battery-ps1.txt"
    kill "$pid"

| 组 | 供电 | 时长 | 命令或文件名变化 | 目标（单核） |
|---|---|---|---|---|
| 面板关闭 | 纯电池 | 300 s | 如上 | ≤ 0.3% |
| 面板关闭 | 插电 | 300 s | 文件名 `battery` 换成 `ac` | ≤ 0.3% |
| 面板打开 | 纯电池或插电，写明 | 120 s | 命令末尾加 `--open-details`，文件名加 `-open`，`sleep 300` 改成 `sleep 120` | ≤ 2% |

- 单核% = CPU 秒差 ÷ 墙钟秒差 × 100。`cputime` 先换算成秒；原始 `cputime` 和 `rss`（KB）都要保留。
- `kill` 结束不是正常退出，报告里写明。
- 面板打开组期间请顺便记下屏幕上是否真的出现了详情面板。
- `--no-probe` 下测出的耗电不含网络探测，这一点在报告里注明。开展探测时的耗电以后由开发者另行评估（阶段 0.2 开探测的 selftest 迷你模式为 0.191% 单核），本轮不要求联网测。

## 4. 单实例（离线）

两个进程必须用**同一个**家目录。

    home=$(mktemp -d)
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse --no-probe &
    pid1=$!
    sleep 3
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse --no-probe
    echo second_exit=$?
    pgrep -x Pulse
    kill "$pid1"

期望：第二个进程马上结束，`second_exit=0`，`pgrep` 只剩 `pid1`。

## 5. 交给用户亲自验收（测试者不做）

用户在 Mac 上直接双击 `mac/build/Pulse.app` 打开。正常打开会经系统代理做网络探测，这是用户自己的选择。然后查看：
1. 菜单栏是否出现两行数值。
2. 左键弹出详情面板；点面板外、按 Esc 都会关闭。
3. 右键菜单：
   - 显示内容的勾选；
   - 组件顺序；
   - 文字颜色；
   - 刷新间隔；
   - 语言切换；
   - 打开日志；
   - 功耗诊断；
   - 退出。
4. 退出后重新打开，设置仍然保持。
5. 可选：关 Wi-Fi 约 20 秒再打开，看右上角断网卡片和恢复卡片；鼠标停在卡片上时它不消失。
6. 可选：登录时打开。

## 6. 交回

把下列内容打成 zip 交回：
- `report.md`，每项写明通过、偏差或未测，附数值与目测描述；
- 四组快照目录；
- 耗电原始记录；
- 单实例输出；
- `build.log`（如果重新构建了）。
