# macOS 阶段 0 真机测试说明

本阶段只有后端与命令行工具，目标为 Apple Silicon、macOS 13+。Windows 端没有编译或运行过；以下所有项目都待真机确认。程序不启动外部进程、不需要 sudo、不写配置、数据或日志文件；下面的文件均由测试者在终端重定向保存。

## 1. 准备和编译

测试包目录必须包含 `mac/` 以及 `src/metrics.h`、`src/power_state.h`、`src/dgpu_state.h`、`src/probe_status.h`。在测试包根目录运行：

```bash
xcode-select -p
# 如果缺少命令行工具，由测试者自行安装：
xcode-select --install

# 安装完成后：
cp -R mac mac.original
set -o pipefail
bash mac/build.sh 2>&1 | tee build.log
```

成功时生成 `mac/build/metrics_selftest`，请保留全部警告。编译失败时只允许修复缺头文件、拼写、类型转换等纯编译错误，并说明理由；不改逻辑、不改共享头。如果需要改逻辑，停止该项并在报告中提出问题。修改后保存补丁，再重新编译：

```bash
diff -ru -x build mac.original mac > fix.patch
# diff 的退出码 1 表示存在差异；不要把它当成编译失败。
bash mac/build.sh 2>&1 | tee build.log
```

不要覆盖原始测试包。没有修改则不需要 `fix.patch`。若系统拦截打开，记录完整提示并交回，不修改系统安全策略。

记录机器信息：

```bash
sw_vers > machine.txt
sysctl -n hw.model >> machine.txt
sysctl -n machdep.cpu.brand_string >> machine.txt
sysctl -n hw.memsize >> machine.txt
sysctl -n hw.logicalcpu >> machine.txt
```

## 2. 三种供电状态

分别在**纯电池、插电充电中、插电已充满或充电暂停**时执行下面一组。将前缀 `battery` 改为 `charging`、`ac-idle` 分别保存，不要覆盖。

```bash
mac/build/metrics_selftest --samples 6 > battery-samples.txt
mac/build/metrics_selftest --power > battery-power.txt
pmset -g batt > battery-pmset.txt
ioreg -rn AppleSmartBattery > battery-ioreg.txt
```

同时截图菜单栏电量与供电状态，写下各命令执行时刻。没有条件达到某种状态时写“未测”，不要临时改变电池优化设置来凑齐。台式机或虚拟机单独运行 `--samples 6`、`--power`，确认 `NoBattery`、电池字段未知且不崩溃；若仅电池查询失败，应显示 `Unknown`。

重点核对：

- 电量与菜单栏一致，负电流对应放电；`power_w` 为正的电池侧功率，不是适配器额定功率。
- 插电时若电池持续净放电，约 10 秒确认后才出现 `battery_supplemental=true`；期间不能把电池功率称为整机功率。确认后短暂跌落有约 5 秒保持时间。
- `eta_s` 只用系统估计；充满、暂停充电或系统不提供估计时可以为 `--`。`eta20_s`、`pct_per_hour`、`avg_w`、`on_battery_s` 本阶段保持未知。
- IOReport 首拍是预热；缺库为 `CounterUnavailable`、缺通道为 `NoMatch`、读取失败或单位不能识别为 `ReadFailed`。这些情况应继续运行。
- `--power` 应列出请求通道及实际样本的组、子组、名称、单位、差分、瓦数和分类，以及电池全部属性和 `PowerTelemetryData`。将未匹配或单位未知的条目完整交回。
- `pkg_w=CPU+GPU+ANE` 是暂定域合计，不含 DRAM，不是整机或适配器输入。任一必要域缺失时合计未知；不能按 0 W 补齐。
- `pkg_energy_delta_pwh` 已从焦耳换成共享接口规定的 pWh；诊断行中的 `delta` 才是通道原始单位。

## 3. CPU、内存、GPU、进程、网速和磁盘

```bash
mac/build/metrics_selftest --samples 10 > compare-samples.txt
mac/build/metrics_selftest --procs > procs.txt
mac/build/metrics_selftest --gpu > gpu.txt
mac/build/metrics_selftest --net > net.txt
```

打开活动监视器，同时截图：

- CPU 与内存页：对照同一时段采样，记录百分点差值。首拍 CPU 无差分，显示 `--` 属正常。
- GPU 历史窗口：`gpu_percent` 来自 IOAccelerator 的设备利用率，不同采样窗口会有波动。缺候选键时应为 `--`，`--gpu` 应仍列出全部实际键。
- CPU 页的前三进程：活动监视器按单核百分比显示，先除以 `hw.logicalcpu` 再与 Pulse 对照。`procs.txt` 开头的“ri_*_time 单位校准”一行说明程序判定的进程时间单位（Mach 时间或纳秒），这是开发端最想确认的数据之一；请顺便找一个活动监视器里接近 100% 的进程（例如跑一个 `yes > /dev/null`，测完 Ctrl+C 结束）对照，确认 Pulse 显示约为 100 ÷ 核心数。本进程不在榜单中；没有权限读取的进程会跳过，`--procs` 打印无法读取、权限拒绝、退出或其他失败的数量。
- 在现有网络条件下下载大文件，对照活动监视器“网络”页的接收/发送速率与 Pulse 的字节/秒。Clash 的 `utun*` 等不会计入，物理 Wi-Fi/以太网才计入。`--net` 应列出接口类型、标志、64 位累计计数及排除原因。
- 磁盘读写量级与活动监视器“磁盘”页对照；`sys_disk_*` 针对 `/System/Volumes/Data`，APFS 共享空间和可清除空间可能造成与 Finder 的口径差异。

`--net` 会并行完整探测四个默认地址，逐个打印 HTTP 状态、响应头耗时和错误；只 2xx 成功，重定向不算成功。它还打印 `nw_path_monitor` 状态（阶段 0 不参与断网判定）。`--no-probe` 用于普通采样时关闭探测，不能与 `--net` 一起使用。

构建脚本把 `metrics_selftest-Info.plist` 嵌入命令行程序，允许需求指定的 HTTP 探测地址（包括 `--url` 自定地址），避免 ATS 把 HTTP 直接拦下。它不修改系统设置，也不在运行时写 plist。请一并保留该构建资源；若出现 `NSURLErrorDomain:-1022`，记录为 ATS 配置未生效，不能误判成真实断网。

Windows 专属或延后指标保持未知：CPU Utility/频率、显存、GPU 温度、Wi-Fi、NVIDIA 独显功率、FPS、电源计划；这不算读取故障。

## 4. 功耗对照（可选）

**只有用户本人同意输入管理员密码时才做**。这条 sudo 命令仅供人工对照，Pulse 本身不会调用它。在两个终端同时运行：

```bash
sudo powermetrics --samplers cpu_power,gpu_power -i 1000 -n 10 > powermetrics.txt
```

```bash
mac/build/metrics_selftest --samples 10 --no-probe > power-compare.txt
```

比对 CPU/GPU/ANE 瓦数，记录相同窗口的平均值和相对误差。功耗接近 0 时不要用相对误差夸大噪声。若当前系统不接受采样器参数，保留原始报错，不安装软件或尝试其他 sudo 命令。

## 5. 自身 CPU 与 Sample 耗时

纯电池，关闭其他大负载，避免同时运行两个测试进程。各运行一遍：

```bash
mac/build/metrics_selftest --compact --samples 150 --quiet > cpu-compact.txt
mac/build/metrics_selftest --samples 300 --quiet > cpu-full.txt
```

各约 5 分钟（首拍立即执行，因此实际稍短）。结束时记录总时长、本进程 CPU 秒数与**单核 CPU%**，这里不除以核心数。quiet 中间不打印，包括网络事件。默认仍有延迟探测，结果包含采样、网络、初始化和结束开销。若需要定位网络成本，可另外加 `--no-probe` 重跑，并标清条件，不用它替代默认验收。

另取少量默认详细输出核对每拍分项耗时，以及最后的 `Sample_ms median/max`：

```bash
mac/build/metrics_selftest --compact --samples 20 > timings-compact.txt
mac/build/metrics_selftest --samples 20 > timings-full.txt
```

分项 `--` 表示该项未执行。Sample total 包含各子项，不能重复相加。单核 CPU 与 Snapshot 的 `self_cpu_percent` 不同：后者除以全部逻辑核心。

## 6. 断网与退出

```bash
mac/build/metrics_selftest --samples 90 > disconnect.txt
```

运行中关闭 Wi-Fi 约 30 秒后打开。记录关闭/打开时间、`Checking/Online/Slow/Offline` 和 `NET` 事件。默认连续 3 次失败或耗时 ≥1000 ms 才断网；恢复需成功且 <1000 ms，较慢阈值默认 500 ms。阶段 0 没有 Windows 的快速确认和退避，nw_path 不直接改判定。

检查退出不会卡到 HTTP 超时：在**关闭 Wi-Fi 的状态下**运行下面的命令，记录 `real` 时间。3 次采样约 2 秒，`real` 明显超过 3 秒就说明退出被探测拖住了。

```bash
time mac/build/metrics_selftest --samples 3 --brief > exit-offline.txt
```

不要为测试另写程序；暂停/恢复、并发调用等接口留到阶段 1 用界面验证，本阶段写“未测”。

## 7. 验收及交回

在 `report.md` 每项写“通过 / 偏差 / 读不到 / 未测”，附实际数值：

| 项目 | 目标 |
|---|---|
| 编译 | arm64、macOS 13+ 编译通过，保留全部警告 |
| 缺数据 | 无电池、IOReport 缺失时不崩溃（未有此机器则未测） |
| 电量 | 与菜单栏一致 |
| CPU、内存 | 同时刻相差不超过 5 个百分点 |
| 网速、进程 | 量级一致，注明单位与核心换算 |
| CPU/GPU 功耗 | 若做对照，相对误差不超过 15% |
| Sample 中位耗时 | 迷你 <2 ms；完整含进程扫描 <10 ms |
| 自身 CPU | 迷你 2 秒刷新，单核不超过 0.3% |
| 退出 | 断网时 `--samples 3` 的 `real` 不超过 3 秒 |

把 `report.md`、全部 stdout/错误输出、截图、`machine.txt`、`build.log`、`fix.patch`（如有）打包为 zip 交回。`--power` 和 `ioreg` 的输出含电池序列号等硬件信息，只交给开发者，不要公开上传。任何目标未达标都写实测值；不能把“代码存在”或“编译通过”写成“读数通过”。不要安装其他软件，不运行上述可选 powermetrics 以外需要 sudo 的命令。
