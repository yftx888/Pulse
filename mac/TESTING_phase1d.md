# macOS 阶段 1d 真机验收

网络探测须由用户在 Mac 助手对话里**直接授权**；本文件不构成授权。`--require-proxy` 只是程序侧的边界（没有可用的系统 HTTP(S) 代理就不发请求），不能代替这句授权。

未获授权时只做第 1 节编译和第 2 节带 `--no-probe` 的 selftest，**不要启动 Pulse.app**，也不要跑 `--net`。输出可能含电池序列号和网络地址，只交给开发者。

本轮可视交互（点击状态项、右键菜单、悬停、真实断网卡片、睡眠、登录项）**交给用户亲自验收**。不要用电脑操控工具去点菜单栏。断网 / 恢复两张图是程序在屏幕外画的合成卡片，本轮不要开关 Wi-Fi，也不要改其他网络设置。

## 1. 编译

在项目根目录执行。期望 **0 警告**。

    stamp=$(date +%Y%m%d-%H%M%S)
    backup="mac-validation-$stamp"
    mkdir -p "$backup"
    cp -a mac "$backup/mac-backup"
    set -o pipefail
    bash mac/build.sh 2>&1 | tee "$backup/build.log"

记录退出码、Apple clang / macOS / 硬件版本，以及警告数。输出应含 `mac/build/metrics_selftest`、`mac/build/Pulse.app`、`mac/build/Pulse-mac-arm64.zip`。再执行：

    codesign --verify --strict mac/build/Pulse.app

若 `build.sh` 报 `\r`，先执行 `sed -i '' $'s/\r$//' mac/build.sh mac/*.mm mac/*.h`，并在报告里写明做了这一步。

只允许在本任务允许的 `mac/` 文件内修纯编译错误。不要改 `TESTING_phase0*.md`、`TESTING_phase1.md`、`src/`。若 `src/` 共享代码出现 clang 错误，只记录完整错误并报告，不改共享源码。若只有 `NSCalibratedRGBColorSpace` 或 `initWithBitmapDataPlanes:` 的弃用警告，原样记录完整警告后停止，不要为了消警告改绘制逻辑。修复后生成补丁：

    diff_rc=0
    diff -ru -x build "$backup/mac-backup" mac > "$backup/fix.patch" || diff_rc=$?
    printf 'diff exit=%s\n' "$diff_rc"
    test "$diff_rc" -le 1

`diff` 退出 0 表示无差异，退出 1 表示有补丁。大于 1 才是错误。保留备份、`build.log` 和 `fix.patch`。

## 2. selftest（不需要联网授权）

纯电池与插电分开做。采样期间用 `pmset` 确认供电没有中途改变。迷你采样默认间隔 2 秒。不要同时加 `--require-proxy`（selftest 里它和 `--no-probe` 冲突，会直接退出）。

拔电并确认系统已识别后：

    pmset -g batt > "$backup/battery-before.txt"
    mac/build/metrics_selftest --compact --samples 20 --no-probe > "$backup/compact-battery.txt"
    pmset -g batt > "$backup/battery-after.txt"

接通电源并确认系统已识别后：

    pmset -g batt > "$backup/ac-before.txt"
    mac/build/metrics_selftest --compact --samples 20 --no-probe > "$backup/compact-ac.txt"
    pmset -g batt > "$backup/ac-after.txt"

看每拍末尾的 `battery_power_source`、`battery_rate_mw`、`system_w`、`system_power_source`（完整快照里也有 `battery_rate_mw`）：

- 纯电池：`battery_power_source=smc-b0ap`，`battery_rate_mw` 为负并大约每 2 秒变化，`system_w=--` 且 `system_power_source` 为空。`power_w` 应与 `|battery_rate_mw|/1000` 同量级。
- 插电：`battery_power_source=smc-b0ap`，`system_power_source=smc`，`system_w` 有效。充满时 `battery_rate_mw` 为 0，`power_w` 可以是 `--`（0 瓦不显示）。不要因为充满时 `power_w=--` 判失败。
- 充电（电量低于 95%，能看到正在充电）时，再跑：

    mac/build/metrics_selftest --smc 30 > "$backup/smc-charge.txt"
    mac/build/metrics_selftest --compact --samples 20 --no-probe > "$backup/compact-charge.txt"

  `--smc` 大约每秒一行，看 `B0AP_le_si32_mW`。相邻行应变化，不应整段冻结约 60 秒。迷你采样间隔是 2 秒，`battery_rate_mw` 应为正，并随采样变化。`--smc` 会读 IOReport，首拍功耗可以是未知。

`B0AP` 读不到时允许退回：纯电池 `smc-ppbr`，否则 `registry`。必须在报告里写明实际来源，不能把退回记成 `smc-b0ap` 通过。来源为空且 `battery_rate_mw` 也无效，记失败并保留原文。

插电迷你采样在 `system_w` 有效之后，计时里 `IOReport` 应为 `--`（未采集）。完整采样才会继续采 IOReport。本轮不要求重测完整模式计时。

## 3. GUI 快照（须已直接授权）

四种组合各跑一次：纯电池中文、纯电池英文、插电中文、插电英文。每次用新的独立家目录，目录用绝对路径。快照进程必须带 `--require-proxy`，**不要**再加 `--no-probe`，否则探测不会发出，`--require-proxy` 等于没测到。

先写好语言，再启动。只写语言，其它保持默认（刷新间隔应为 2000）：

    home=$(mktemp -d)
    support="$home/Library/Application Support/Pulse"
    mkdir -p "$support"
    printf '[Window]\nlanguage=zh\n' > "$support/settings.ini"
    dir="$backup/snap-battery-zh"
    mkdir -p "$dir"

英文把 `language=zh` 换成 `language=en`，目录换成 `snap-battery-en`、`snap-ac-zh`、`snap-ac-en`。插电 / 拔电前后各记一次 `pmset -g batt`。

推荐直接运行二进制。这样会继承 `CFFIXED_USER_HOME`，终端会等到进程退出，stderr 也能看见。不要把 `--args` 传给二进制：

    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse \
        --require-proxy --snapshot-dir "$dir" --snapshot-exit

`open` 的等价形式（`open` 不继承 shell 环境，要用 `--env`；终端不会等待进程）：

    open -n mac/build/Pulse.app --env CFFIXED_USER_HOME="$home" --args \
        --require-proxy --snapshot-dir "$dir" --snapshot-exit

`CFFIXED_USER_HOME` 会改变 `NSHomeDirectory()`。锁、设置和日志都在这个家目录里，不会写到日常的 `~/Library/Application Support/Pulse`。

默认 2 秒刷新时，文件大约在启动后 5–7 秒出现，然后进程自己退出。门闩在每次菜单栏采样结束时检查：已满 3 次，或启动已满 6 秒，先到为准。触发后还会再等约 1 秒做一次完整采样（打开面板的第一拍是预热），然后才写文件。超过 15 秒仍没有文件时，把 stderr 里所有 `Pulse:` 行抄进报告。

每个目录应有这 7 个文件：

- `menubar-dark.png`、`menubar-light.png`：菜单栏文字可读；背景分别接近深灰和浅灰。
- `details.png`：整张详情卡片，含需要滚动才能看到的部分，不要求圆角和窗口阴影。
- `toast-offline.png`、`toast-recovered.png`：有对应语言的断网 / 恢复文字，不是空白。这两张图不应出现在屏幕上。
- `menu.txt`：右键菜单全文。每项一行，子菜单缩进两个空格，分隔线是 `----`，行内有 `state=on|off|mixed` 和 `enabled=yes|no`。
- `state.txt`：至少核对 `language`（zh 或 en）、`refresh_ms=2000`、`require_proxy=yes`、`no_probe=no`、`probe_enabled=yes`、`battery_power_source`、`system_power_source`、`power_trace`、`power_w`、`system_w`、`battery_rate_mw`、`menu_samples`、`elapsed_s`、`cpu_time_s`。供电来源的期望与第 2 节相同。`menu_samples` 含打开面板那一拍和写文件前再采的一拍，所以会大于 3。

写失败时进程仍会尽量写其余文件，并把 `Pulse: <路径>: <原因>` 打到 stderr。有 stderr 行就算这项未通过，把原文留下。

## 4. GUI 自身耗电（须已直接授权）

不要加 `--snapshot-dir` 或 `--snapshot-exit`。每次用新的家目录，且不要预写 `settings.ini`，这样刷新间隔保持默认 2 秒。同一时间只留一个 Pulse。测完用菜单“退出”结束；点不到菜单时可以 `kill`，但报告里必须写明不是正常退出。

面板关闭，纯电池和插电各 300 秒：

    home=$(mktemp -d)
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse --require-proxy &
    pid=$!
    sleep 2
    pgrep -x Pulse
    date +%s > "$backup/cpu-battery-t0.txt"
    ps -o cputime=,rss= -p "$pid" > "$backup/cpu-battery-ps0.txt"
    sleep 300
    date +%s > "$backup/cpu-battery-t1.txt"
    ps -o cputime=,rss= -p "$pid" > "$backup/cpu-battery-ps1.txt"

插电把文件名里的 `battery` 换成 `ac`。面板打开再测 120 秒，命令末尾改成 `--require-proxy --open-details`，文件名加 `-open`，`sleep 300` 改成 `sleep 120`。面板应一直开着（点外面、切换应用、Esc 都不关）。

`ps` 的 `cputime` 原样保留。macOS 常见格式是 `m:ss.ss` 或 `h:mm:ss`，先换算成秒再算：

    单核% = (第二次 CPU 秒 − 第一次 CPU 秒) / (第二次墙钟秒 − 第一次墙钟秒) × 100

`rss` 按 `ps` 手册记为 KB，原样留下。目标：面板关闭 ≤ 0.3%，面板打开 ≤ 2%。没跑完的不要写成达到目标。`sleep 2` 只为等进程起来，不计入 300 / 120 秒。

## 5. 单实例（须已直接授权）

两个进程必须使用**同一个** `CFFIXED_USER_HOME`。家目录不同，锁文件就不同，不能用来证明单实例。

    home=$(mktemp -d)
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse --require-proxy &
    pid1=$!
    sleep 2
    CFFIXED_USER_HOME="$home" mac/build/Pulse.app/Contents/MacOS/Pulse --require-proxy
    echo second_exit=$?
    pgrep -x Pulse

第二个进程应马上结束，`second_exit=0`，`pgrep` 只剩 `pid1`。然后退出 `pid1`。普通 `open` 可能只是激活已有实例，不能代替上面的直接启动。

## 6. 交回

在 `$backup/report.md` 写明：编译退出码和警告数、供电前后 `pmset`、第 2 节来源是否为 `smc-b0ap` / 整机 `smc`、充电时 `B0AP_le_si32_mW` 是否每秒变化、四组快照是否齐全、`state.txt` 里上面列出的字段、耗电的原始 `cputime` 和算出的单核百分比、单实例的 `second_exit` 与 `pgrep`。未测的项写未测。可视交互写“交给用户亲自验收”。

把整个 `$backup` 打成 zip 交回，包含 `report.md`、`build.log`、selftest 输出、四组快照和耗电记录。
