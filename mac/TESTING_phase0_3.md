# macOS 阶段 0.3 补测说明（2026-10-28）

**网络探测需要用户在给测试者的任务说明里直接授权，本文件不构成授权。** 未收到用户直接授权时，带 `--require-proxy` 的各项改用 `--no-probe` 并在文件名与报告里标明，`--net` 记“未测”。

本轮只补 0.2 没有覆盖的项目，**不要重跑 0.2 的纯电池各项**。与 0.2 相比，代码改了两处：`--net` 输出新增“经代理端点”一列；新增只读诊断 `--smc`（第 5 节，本轮重点，需要插电、纯电池各跑一次）。程序只输出 stdout，不调用外部进程、不需要 sudo、不写文件；不安装软件，不改代理、电源或安全设置。各项顺序运行，不并行。

## 1. 编译

~~~bash
cp -R mac mac.original
set -o pipefail
bash mac/build.sh 2>&1 | tee build.log
~~~

只修纯编译错误并保存 `fix.patch`（`diff -ru -x build mac.original mac > fix.patch`），不改逻辑。

## 2. 代理端点确认（需直接授权）

~~~bash
mac/build/metrics_selftest --net --require-proxy > net-require-proxy.txt 2>&1
~~~

0.2 中四个地址都是 `remote=127.0.0.1 port=7897`，但系统指标 `proxyConnection` 显示“否”。本版按“远端地址和端口是否就是本轮显式代理”判断，新增 `经代理端点=` 一列。验收：四行 `ok=1` 且 `经代理端点=是`，没有“警告：该请求未经代理”。

## 3. 插电（AC）性能

插电，满电或充电暂停均可（记录 `pmset -g batt`）。**每项前后记录供电状态；中途供电变化或测试中断则整项作废重测，不拼接输出。**

~~~bash
pmset -g batt > ac-power-before.txt
mac/build/metrics_selftest --compact --samples 20 --require-proxy > ac-compact-timings.txt 2>&1
mac/build/metrics_selftest --samples 20 --require-proxy > ac-full-timings.txt 2>&1
mac/build/metrics_selftest --compact --samples 150 --quiet --require-proxy > ac-compact-cpu.txt 2>&1
pmset -g batt > ac-power-after.txt
~~~

交回 `Sample_ms` 中位与最大值、各分项中位，以及 150 拍的单核 CPU%（**不除以核心数**）。插电迷你模式每拍都做 IOReport 采样，预计中位约 2–3 ms，如实记录。耗电目标：迷你插电 ≤ 0.3% 单核。

## 4. 插电遥测（可选）

~~~bash
mac/build/metrics_selftest --telemetry 150 > ac-telemetry.txt 2>&1
~~~

0.2 在纯电池下观察到 `SystemLoad` 约每 60 秒更新一次。本项确认插电时的周期：列出每次变化的 t_s 和相邻间隔；变化不足两次就写“观察不足”。

## 5. SMC 功耗（重点，插电与纯电池各一次）

0.2 发现电池功率和 `SystemLoad` 都约 60 秒才更新。本项检查 SMC 里的功率传感器（如 `PSTR` 整机、`PPBR` 电池、`PDTR` 输入）是否更快。诊断**只读**：只发“读键信息 / 按索引读键名 / 读字节”三种命令，不写 SMC，不需要 sudo，不联网。

~~~bash
pmset -g batt > ac-smc-power-before.txt
mac/build/metrics_selftest --smc 60 > ac-smc.txt 2>&1
pmset -g batt > ac-smc-power-after.txt
# 拔电，确认纯电池后：
pmset -g batt > battery-smc-power-before.txt
mac/build/metrics_selftest --smc 60 > battery-smc.txt 2>&1
pmset -g batt > battery-smc-power-after.txt
~~~

每次约 1 分钟。输出开头是 SMC 打开结果、键总数和全部 `P` 开头的键；随后每秒一行候选键的数值、原始字节、读取耗时，并附同一时刻的 `InstantAmperage` / `Voltage`；最后是每个键的变化次数与变化间隔。交回：
- 哪些键在**纯电池**时数值接近电池放电功率（可用同一行的 `InstantAmperage × Voltage / 1e6` 粗略对照，单位 W），以及它的变化间隔中位数；
- 插电时 `PSTR`、`PDTR` 的值与变化间隔；
- 单次读取耗时（read_ms）中位数。
运行中不要做其他负载；供电中途变化则该次作废重测。

## 6. 活动监视器同步（能读屏幕才做，否则记未测）

内存（决定 Pulse 的“已用内存”采用哪种口径）：

~~~bash
mac/build/metrics_selftest --mem > mem.txt 2>&1
~~~

运行时打开活动监视器“内存”页。程序每 2 秒输出一块、共 5 块；**每块输出时**记下活动监视器底部的“已使用内存 / App 内存 / 联动内存 / 被压缩 / 已缓存文件 / 已使用的交换”六个数和时间，注明显示单位（GB）。读不到同一时刻的数就写明相差几秒。

进程前三：

~~~bash
mac/build/metrics_selftest --samples 10 --require-proxy > top3.txt 2>&1
~~~

运行时活动监视器切到“CPU”页、“所有进程”视图，按 %CPU 排序，至少记 3 次前三名及其百分比和时间。比较时把活动监视器的数值除以 10（M5 为 10 核）。

## 7. 交回

全部输出、`report.md`（每项写通过 / 偏差 / 未测，附数值、供电状态和探测口径）、`build.log`、`fix.patch`（如有）。输出可能含硬件序列号和网络地址，只交给开发者，不公开上传。
