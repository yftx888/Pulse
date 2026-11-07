# macOS 阶段 1 / 1b / 1c 真机验收

网络探测须由用户在 Mac 助手对话里**直接授权**；本文件不构成授权。未获授权时只运行带 --no-probe 的 selftest、离线诊断和静态构建检查，**不要启动 Pulse.app**（程序启动会启动网络探测）。输出可能含序列号与网络地址，只交给开发者。

## 1. 编译

在项目根目录执行：

    stamp=$(date +%Y%m%d-%H%M%S)
    backup="mac-validation-$stamp"
    mkdir -p "$backup"
    cp -a mac "$backup/mac-backup"
    set -o pipefail
    bash mac/build.sh 2>&1 | tee "$backup/build.log"

记录退出码、Apple clang / macOS / 硬件版本及警告数。

输出应含 mac/build/metrics_selftest、mac/build/Pulse.app、mac/build/Pulse-mac-arm64.zip；运行 codesign --verify --strict mac/build/Pulse.app。只允许在本任务允许的 mac/ 文件内修纯编译错误（禁止修改 TESTING_phase0*.md），不要顺手改逻辑。若 src/ 共享代码出现 clang 错误，只记录完整错误并报告开发者，不修改共享源码。修复后在项目根目录生成补丁：

    diff_rc=0
    diff -ru -x build "$backup/mac-backup" mac > "$backup/fix.patch" || diff_rc=$?
    printf 'diff exit=%s\n' "$diff_rc"
    test "$diff_rc" -le 1

diff 退出0表示无差异，退出1表示发现差异、属于补丁生成的预期结果；大于1才是错误。保留备份、build.log 与 fix.patch。

## 2. selftest（纯电池与插电分别做）

在项目根目录、确认已拔下电源后执行；前后状态文件用于确认采样期间供电轨迹没有改变：

    pmset -g batt > "$backup/battery-before.txt"
    mac/build/metrics_selftest --compact --samples 20 --no-probe > "$backup/compact-battery.txt"
    mac/build/metrics_selftest --power > "$backup/power-battery.txt"
    mac/build/metrics_selftest --mem > "$backup/memory-battery.txt"
    mac/build/metrics_selftest --smc 30 > "$backup/smc-battery.txt"
    mac/build/metrics_selftest --samples 20 --no-probe > "$backup/full-battery.txt"
    pmset -g batt > "$backup/battery-after.txt"

接通电源并确认系统已识别后执行，保留另一组独立文件：

    pmset -g batt > "$backup/ac-before.txt"
    mac/build/metrics_selftest --compact --samples 20 --no-probe > "$backup/compact-ac.txt"
    mac/build/metrics_selftest --power > "$backup/power-ac.txt"
    mac/build/metrics_selftest --mem > "$backup/memory-ac.txt"
    mac/build/metrics_selftest --smc 30 > "$backup/smc-ac.txt"
    mac/build/metrics_selftest --samples 20 --no-probe > "$backup/full-ac.txt"
    pmset -g batt > "$backup/ac-after.txt"

纯电池 battery_power_source=smc，system_w=-- 且整机来源为空；插电电池功率来源为空，system_w 有效且 system_power_source=smc。SMC 不支持/失败时允许退回 registry 或处理器，但必须明确记录。

插电 compact 首拍允许 IOReport 预热；取得有效 PSTR 后，后续计时 IOReport=--（未采集），中位目标接近纯电池约 0.4 ms。完整采样的 IOReport 应继续采集。不要把未测目标记成结果。

--mem 的 F 与活动监视器“已使用内存”同步对照，目标差 ≤ 0.1 GiB；记录活动监视器原始GB数值及单位，若采用十进制GB先乘10^9/2^30转GiB再比较，不直接混用。原有 A–E 仍在。SMC 每行应含 B0AP 原始字节、小端 si32 mW、IOReport CPU/GPU/ANE/DRAM W；IOReport 首拍未知，后续真实差分。采样为顺序读取，并非原子同步。

## 3. 启动与单实例（先取得直接联网授权）

以下全部GUI操作和自身CPU测量均依赖App运行；未获直接联网授权时，第3–10项全部暂不执行。

    open mac/build/Pulse.app
    pgrep -x Pulse
    open -n mac/build/Pulse.app
    sleep 2
    pgrep -x Pulse

确认仅一个 Pulse 进程；普通 open 可能只激活原实例，不能验证 flock。截图菜单栏深色/浅色外观（可切换时），记录刘海屏是否隐藏内容。程序不应出现 Dock 图标。

## 4. 读数与组件

纯电池“整机”约每 1–2 s 变化，与同期 --smc PPBR 同量级；等待续航出现，持续 60 s 以上后查平均功耗。插电后菜单栏仍是“整机”，与 PSTR 同量级；详情硬件“处理器 W”继续使用 IOReport。插拔电、充满和插电放电如能复现，分别记录标签、来源与曲线切换。

启用菜单栏 GPU 组件，保持面板关闭，确认 GPU 有读数且会变化；取消后不应继续采 GPU。菜单里不应提供 CPU 频率、FPS、GPU 温度、显存、Wi-Fi。

菜单栏深色/浅色分别检查配色：充电电量/续航/功耗为绿，接电待机电量为绿，普通放电功耗为黄、低电量按警示规则为红；补充供电优先黄。上传紫/下载蓝，磁盘读蓝/写紫，CPU 蓝、内存紫、GPU 绿，正常延迟绿/较慢黄/断网红/检测中弱化。浅色应为相同色相的深色版本；自定义颜色与 PreserveValueColor 警示优先规则不变。

## 5. 详情面板与菜单

左键打开立即采样，随后每秒更新；截图所有卡片（长面板可滚动）。检查硬件四格、电池、走势、网络、进程前三、磁盘、系统盘、系统信息、电池健康。普通权限无法读到的系统进程不要求出现在前三。

点外面、Esc、再次左键应关闭；原应用保持前台；Pulse 自身被激活时不应误关面板，切到其他应用仍关闭。确认电池卡片不出现“电源计划 · 未知”及其预留宽度，低电量模式仍显示 saverLabel。右键与 Control+单击打开菜单。全选/全不选/恢复默认、组件逐项勾选、顺序四种移动、颜色选择/恢复默认、刷新间隔 1/2/5 s、延迟阈值、提醒和提示音开关、语言、打开日志、功耗诊断、退出都要检查。功耗诊断应后台完成，GUI 继续刷新，结果写入 ~/Library/Logs/Pulse/ 并自动打开。

先调整组件顺序和勾选，再用菜单栏“显示内容 → 恢复默认”，确认顺序和勾选一起恢复。中文切到英文、减少列或改变顺序后，列宽应重新按文字计算，不沿用此前较宽值；稳定数字变化时不反复收缩。面板开关时 CPU、网络和电池统计保留连续区间，硬件 IOReport 会重新预热一拍，允许短暂“采样中”或“--”。

## 6. 设置持久化

改勾选、顺序、颜色、间隔和语言后退出重开，对照 ~/Library/Application Support/Pulse/settings.ini。应沿用 [Taskbar] / [Full] / [Window] / [Network] 键。备份 ini 后测试未知值回默认；检查没有遗留 settings.ini.XXXXXX 临时文件。不要覆盖用户日常设置。

在备份并隔离的测试配置中，分别使用 UTF-8、带 BOM 的 UTF-16LE/BE、GB18030 文件，检查中文值和未知节/键读入后保存仍保留。预填 [Taskbar] 的 cpu_freq/fps/gpu_temp 及其 color_ 键、[Full] 的 gpu_memory/wifi/fps，保存后这些不支持键的原值应保留；原来没有的键不应新增。

另用三种编码均不能解码的字节文件（如单字节 0x81），记录哈希；程序应按默认值运行、stderr 仅提示一句，不弹窗退出。多次改设置、拖颜色、退出后文件哈希应保持不变；即使运行期间把文件换成可读内容，也须重启才恢复保存。目录无法创建仍按原方式报错。

连续拖动取色面板时检查只有内存颜色/画面变化；停止变化约 0.5 s 后才保存一次，不应每次 colorChanged 都创建临时文件/fsync 或触发重采样。拖动后立刻退出，应保存最后一次颜色且不遗留计时器。

## 7. 网络提醒（需直接授权）

关 Wi-Fi 约 20 s 再打开，记录实际断网、判定、恢复时间并截图。提醒位于状态项所在屏幕 visibleFrame 右上角、菜单栏下方，右侧/顶部边距均 12 pt；打开详情时新事件提醒不应再贴在状态项下方覆盖详情。多屏分别测试。卡片可点击关闭；完整显示满 8 s 后开始约 0.4 s 淡出。悬停超过 8 s 不消失，移开重新等待 8 s；淡出中移入恢复完整不透明并暂停，移开重新计时。新事件替换旧卡片，旧计时器不得提早关闭新卡片。断网提醒关闭时不弹卡片，声音开关分别生效。

关闭断网提醒、取消菜单栏延迟并关闭详情面板，确认日志/连接不再产生新 HTTP 探测；保持网络卡片勾选但详情关闭仍不探测。单独打开提醒、勾选延迟、或打开带网络卡片的详情时各自都应恢复探测；关闭详情或取消网络卡片后再次停止。以三项全关配置重启，应从启动前即禁用探测，不先发一轮。

## 8. 睡眠与屏幕

合盖或运行 pmset sleepnow 前先问用户。醒来后读数与计时恢复，日志不把睡眠算断网。屏幕睡眠期间不应继续界面计时，亮屏后恢复。观察电池统计不把睡眠间隔积分。唤醒后的下一拍应关闭旧 SMC 连接并重新打开，PPBR/PSTR 继续更新。若可在只读测试桩中注入 IOConnectCallStructMethod 的内核错误，确认连接关闭、键退避清空、60 s 后重开；SMC result 非 0、类型/范围错误只退避对应键，不关闭共享连接。

## 9. 登录项（操作前取得用户许可）

初始不默认注册；勾选后看“系统设置 → 通用 → 登录项”，退出再开保持实际状态，取消后消失。若 requiresApproval，点击菜单应提示并打开系统设置，不应静默注销；批准或拒绝后再查菜单真实状态。不要修改其他登录项。

## 10. 自身耗电

关闭 selftest 和其他临时 Pulse。用 pgrep -x Pulse 取 PID，执行 ps -o cputime=,rss= -p <pid>，隔 300 s 重复；cputime 转成秒。单核 CPU% = CPU 秒差 / 实际秒差 × 100。

菜单栏默认组件、2 s 刷新，纯电池和插电各测 300 s，目标 ≤ 0.3%；再测 1 s。详情面板打开按 1 s 测 120 s，目标 ≤ 2%。同时记录活动监视器“内存”列、后台负载以及 SMC 是否正常；不要把 RSS 当活动监视器内存直接等同。

## 阶段 1c 保留的限制

插电电池卡片的充电瓦数/充满时间仍来自 registry V×I，可能约 1 分钟更新；对照本轮 --smc 每秒 B0AP 原始字节/si32 记录后再决定是否换源。屏幕睡眠期间丢弃断网事件、第二实例不唤起原实例、IOPS 电量缓存 10 s 本轮未改。Esc 与非激活面板的键盘行为、macOS SDK 弃用警告仍需真机确认。

## 11. 交回

report.md 对每项写通过 / 偏差 / 未测，附数值、截图、硬件和系统版本；交回 build.log、fix.patch（如有）、全部输出。代码未在 Windows 主机做过 Mac 编译或真机验收，此处步骤均待执行。
