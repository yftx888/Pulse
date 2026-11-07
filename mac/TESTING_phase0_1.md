# macOS 阶段 0.1 补测说明（2026-10-28）

仅补测本轮改动。Apple Silicon、macOS 13+；程序只向 stdout 输出，不调用外部进程、不需要 sudo、不写配置或数据文件。下面的输出文件由终端重定向保存。不要重跑上一轮所有项目，不修改系统安全策略或电源设置。

## 1. 编译

测试包需同时包含 mac/ 和 src/ 下 metrics.h、power_state.h、dgpu_state.h、probe_status.h 四个共享头。包根目录执行：

~~~bash
cp -R mac mac.original
set -o pipefail
bash mac/build.sh 2>&1 | tee build.log
~~~

保留原始包和所有警告。只允许修缺头、拼写、类型转换等纯编译错误；不改逻辑、不改共享头。需改逻辑则停止该项、报告问题。修过后保留原始失败日志，再保存补丁并重新构建：

~~~bash
diff -ru -x build mac.original mac > fix.patch
# diff 返回 1 仅表示有差异。
bash mac/build.sh 2>&1 | tee build-fixed.log
~~~

没有修改则不需 fix.patch。report.md 记下机型、芯片、内存、macOS/clang 版本和逻辑核数（本机 M5 为 10）。

## 2. 网络探测（本轮必须补测）

默认探测只访问以下四个公开连通性检测地址，沿用系统代理，口径与 Windows 版相同；不要传 --url：

- http://www.msftconnecttest.com/connecttest.txt
- http://www.gstatic.com/generate_204
- http://connect.rom.miui.com/generate_204
- http://captive.apple.com/hotspot-detect.html

**用户已于 2026-10-28 明确允许本轮运行网络探测**（原话“允许探测”），包括 `--net`、默认开启探测的采样、断网与退出测试，不需要再请示。本轮所有普通采样都**不要加 --no-probe**。不要修改代理、路由或 Wi-Fi 以外的网络设置；断网测试只开关 Wi-Fi。

~~~bash
mac/build/metrics_selftest --net > net.txt 2>&1
mac/build/metrics_selftest --samples 90 > disconnect.txt 2>&1
~~~

net.txt 开头显示 HTTP/HTTPS 的启用状态、代理主机/端口、PAC 设置。逐 URL 查看 proxy=是/否/未知、remote、port：是/否来自该任务最后一个 transactionMetrics，未知表示回调未及时到达或未提供指标，**不能把未知当成绕过代理**。系统代理配置并不保证每个地址的实际路由；若 proxy=否 与预期冲突，记下 PAC/规则与输出，交回开发者，不自行改路由或探测逻辑。

90 拍运行期间，关闭 Wi-Fi 约 30 秒再打开，记录开关时刻、Offline/恢复事件及恢复延迟。再保持 Wi-Fi 关闭，做一次退出检查，保存 time 的 stderr 后恢复 Wi-Fi：

~~~bash
{ time mac/build/metrics_selftest --samples 3 --brief; } > exit-offline.txt 2>&1
~~~

3 拍约 2 秒，real 应不超过约 3 秒；超出则记实测值。

## 3. 分项耗时

**插电（AC）和纯电池各一组**，将前缀 ac 改为 battery。安静环境，避免并行跑其他测试：

~~~bash
mac/build/metrics_selftest --compact --samples 20 > ac-compact-timings.txt 2>&1
mac/build/metrics_selftest --samples 20 > ac-full-timings.txt 2>&1
~~~

交回逐拍 timing_ms 和末尾 Sample_ms median/max。关注 IOReport sample/parse、Battery registry/IOPS；总项包含子项，不能重复相加。纯电池迷你首拍之后 IOReport 通常关闭，显示 -- 正常。目标：**AC 迷你（IOReport 开启）中位 <2 ms，完整 <10 ms**；未达标记实际数值。

## 4. 自身 CPU

插电和纯电池各做一次迷你，纯电池另做一次完整。关闭其他大负载，不同时运行两个测试实例；探测保持默认开启（用户已允许）。

~~~bash
mac/build/metrics_selftest --compact --samples 150 --quiet > ac-compact-cpu.txt 2>&1
# 拔电后：
mac/build/metrics_selftest --compact --samples 150 --quiet > battery-compact-cpu.txt 2>&1
mac/build/metrics_selftest --samples 300 --quiet > battery-full-cpu.txt 2>&1
~~~

每次约 5 分钟。记录总时长、CPU 秒数、**单核 CPU%（不除以 10）**。AC 迷你目标 ≤0.3%；本轮开启探测的结果不能直接套用上一轮 --no-probe 的 0.133%。

## 5. IOReport 全组 / 过滤对照

插电、纯电池各一次，将前缀 ac 改为 battery：

~~~bash
mac/build/metrics_selftest --power > ac-power.txt 2>&1
~~~

核对 requested（全组）、filtered-request（计划过滤）、subscribed（实际订阅）通道及 fallback_full。M5 正常应只订阅 CPU Energy、与 CPU 同 driver 的 GPU、ANE、DRAM，不能把 GPU 和 GPU Energy 相加。若回退全组或缺通道，报告实际列表。

交回 benchmark-full 与 benchmark-filtered 两行的 sample_ms / parse_ms 共四个数：每臂 CreateSamples 连续 6 次，丢首拍取后 5 次中位；另对这 6 份样本做 5 次差分+解析取中位。benchmark-last-delta 的极短窗口可能为 0 W，**用 compare-full / compare-filtered 的约 1 秒重叠窗口核对 CPU/GPU/ANE/DRAM 瓦数**，记差值及 interval_s；不要把不同瞬间的小幅波动写成严格相等。full 行还保留全组每通道差分。

## 6. 整机遥测

插电满电、纯电池各一次；充电中能做到才做，不为凑状态改电池优化设置。前缀按 ac-idle / battery / charging 分开保存：

~~~bash
mac/build/metrics_selftest --telemetry 60 > ac-idle-telemetry.txt 2>&1
# 充电中可顺便：
mac/build/metrics_selftest --samples 6 > charging-samples.txt 2>&1
~~~

默认 30 次，允许 5–600，每秒一行。每行 t_s 为 steady clock 相对诊断启动秒数（3 位小数），read_ms 仅 PowerTelemetryData 一次读取，不含匹配与两个供电标志读取。记录八个瞬时字段、三组原始累加值/计数和 *_avg_mW，以及 ExternalConnected/IsCharging；-- 为缺失、无有效相邻样本或计数不递增。结束有 changes/valid_pairs，可判断更新频率。比较 SystemLoad、SystemPowerIn、BatteryPower 与累加差平均值，**累加字段的单位/意义仍待本轮确认，尚未进入 Snapshot**。

## 7. 内存口径

~~~bash
mac/build/metrics_selftest --mem | tee mem.txt
~~~

同时打开活动监视器“内存”页。程序每 2 秒输出一个带 t_s 的块，共 5 次；每次把该时刻的 **已使用内存 / App 内存 / 联动内存 / 被压缩 / 已缓存文件 / 交换** 六个数与时间记入 report.md（可截图）。输出含 hw.memsize、两种页大小、vm.swapusage、VM 原始页数及 GiB，以及 A–E 候选口径。注意 GiB 与活动监视器显示单位差异；累计事件乘页大小不是当前内存占用。Snapshot 仍沿用原 A 口径，本轮没有改。

## 8. 进程前三

~~~bash
mac/build/metrics_selftest --samples 10 > top3.txt 2>&1
~~~

活动监视器切到“所有进程”，同时对照 top3。活动监视器为单核百分比，**除以 10（或实际逻辑核数）** 再比较；不要改已验证的进程时间单位校准。

## 9. 交回

全部 stdout/stderr、report.md（逐项写通过 / 偏差 / 未测并附数值与供电状态）、build.log、build-fixed.log/fix.patch（如有）、必要截图打包 zip。任何性能目标未达到都记实测值。无电池/私有库不可用等降级若没有对应环境，注明未测。

输出含电池序列号和网络地址，**只交给开发者，不公开上传**。不安装其他软件，不执行 sudo。
