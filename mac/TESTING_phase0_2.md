# macOS 阶段 0.2 补测说明（2026-10-28）

**网络探测需要用户在给测试者的任务说明里直接授权，本文件不构成授权。** 未收到用户直接授权时，第 2 节网络各项记“未测”，其余照做；旧说明中的授权不能替代本轮直接授权。

Apple Silicon、macOS 13+。程序只输出 stdout，不调用外部进程、不需要 sudo、不写配置、数据或日志文件；下面的结果文件由终端重定向保存。各项顺序运行，不并行；不安装软件，不改安全策略、电源设置或代理配置。网络测试只开关 Wi-Fi。

## 1. 编译与口径

包内须有 mac/ 和 src/metrics.h、power_state.h、dgpu_state.h、probe_status.h。在包根目录执行，保留原包：

~~~bash
cp -R mac mac.original
set -o pipefail
bash mac/build.sh 2>&1 | tee build.log
~~~

目标首次编译通过、0 警告。只修缺头、拼写、类型转换等纯编译错误，不改逻辑或共享头；需要改逻辑则停止该项并报告。修过后保留原失败日志，再保存补丁和新日志：

~~~bash
diff -ru -x build mac.original mac > fix.patch
# diff 返回 1 仅表示有差异。
bash mac/build.sh 2>&1 | tee build-fixed.log
~~~

report.md 记录机型、芯片、内存、macOS/clang 版本、逻辑核数（M5 为 10）。以下变量默认关闭探测；只有收到用户直接授权后才改成第二组：

~~~bash
probe_flag=--no-probe
probe_tag=no-probe
# 已获直接授权时，改为：
# probe_flag=--require-proxy
# probe_tag=require-proxy
~~~

本地参数检查不发请求：同时传 --require-proxy --no-probe 应显示用法、退出码 2。

~~~bash
mac/build/metrics_selftest --require-proxy --no-probe > usage-conflict.txt 2>&1
printf 'exit=%s\n' "$?" >> usage-conflict.txt
~~~

## 2. 网络（仅获直接授权时执行）

全部加 --require-proxy，不传 --url。四个默认地址为 msftconnecttest、gstatic、connect.rom.miui、captive.apple 的连通性检测 URL。每轮请求前检查所需 HTTP(S) 系统代理，不满足则不建 session、不发请求；不要改代理配置来凑成功。

~~~bash
mac/build/metrics_selftest --net --require-proxy > net-require-proxy.txt 2>&1
mac/build/metrics_selftest --samples 90 --require-proxy > disconnect-require-proxy.txt 2>&1
~~~

交回系统代理、本轮实际使用主机/端口，以及每个 URL 的 HTTP 状态、ok、proxy、耗时与错误。默认 URL 均为 HTTP。如系统本来未配置所需代理，保留拒绝结果，确认独立拒绝标志为真、timeout 为假，其他联网项记未测；不要把代理拒绝当超时或真实断网。成功行应 ok=1 且 proxy=是；proxy=未知 不算已验证走代理，proxy=否 会额外打印“警告：该请求未经代理”，两者均交回偏差。

90 拍期间关闭 Wi-Fi 约 30 秒再打开，记下开关时间、Online/Slow/Offline 变化、NET 事件及恢复时刻。目标连续 3 次失败内判 Offline；恢复后回到 Online/Slow 并发恢复事件。不要把 Wi-Fi 已连上等同网络已恢复。

保持 Wi-Fi 关闭，做退出检查后再打开 Wi-Fi：

~~~bash
{ time mac/build/metrics_selftest --samples 3 --brief --require-proxy; } > exit-offline-require-proxy.txt 2>&1
~~~

交回 real 时间，目标 ≤3 秒。不能关闭 Wi-Fi 或未获直接授权时，对应项记未测。

## 3. 分项耗时

插电满电、纯电池各一组，将下方 ac-idle 改为 battery：

~~~bash
pmset -g batt > "ac-idle-timings-${probe_tag}-power-before.txt"
mac/build/metrics_selftest --compact --samples 20 "$probe_flag" > "ac-idle-compact-timings-${probe_tag}.txt" 2>&1
mac/build/metrics_selftest --samples 20 "$probe_flag" > "ac-idle-full-timings-${probe_tag}.txt" 2>&1
pmset -g batt > "ac-idle-timings-${probe_tag}-power-after.txt"
~~~

交回逐拍 timing_ms、Sample_ms median/max，关注 Battery registry/IOPS 和 Network throughput。IOPS 只在首次、≥10 秒或供电标志变化/读取失败等刷新拍出现，其余为 --；电量与系统 ETA 最多约 10 秒缓存，分类和功率仍读取本拍 registry。总项含子项，不能重复相加。插电迷你仍逐拍做 IOReport 内核采样，预计中位 >2 ms，如实记录，**不据此判本轮失败**；纯电池迷你预热后 IOReport 为 -- 正常。

## 4. 自身 CPU（每项约 5 分钟）

**每项前后分别记录供电状态。中途供电变化或测试中断则整项作废重测，不拼接输出。** 关闭其他大负载，一次只运行一个测试实例。flag/tag 使用第 1 节的授权口径：

~~~bash
pmset -g batt > "ac-compact-cpu-${probe_tag}-power-before.txt"
mac/build/metrics_selftest --compact --samples 150 --quiet "$probe_flag" > "ac-compact-cpu-${probe_tag}.txt" 2>&1
pmset -g batt > "ac-compact-cpu-${probe_tag}-power-after.txt"

# 拔电，确认纯电池后：
pmset -g batt > "battery-compact-cpu-${probe_tag}-power-before.txt"
mac/build/metrics_selftest --compact --samples 150 --quiet "$probe_flag" > "battery-compact-cpu-${probe_tag}.txt" 2>&1
pmset -g batt > "battery-compact-cpu-${probe_tag}-power-after.txt"

pmset -g batt > "battery-full-cpu-${probe_tag}-power-before.txt"
mac/build/metrics_selftest --samples 300 --quiet "$probe_flag" > "battery-full-cpu-${probe_tag}.txt" 2>&1
pmset -g batt > "battery-full-cpu-${probe_tag}-power-after.txt"
~~~

交回总时长、本进程 CPU 秒数、**单核 CPU%（不除以核心数）**。开启探测时迷你插电/电池目标均 ≤0.3%；完整电池记录实测值。无授权的 --no-probe 结果不能替代开启探测验收。上一轮电池完整 300 拍因断连作废，**本轮必须补上**；无法完成则明确未测。

## 5. 遥测

插电满电、纯电池分别执行，将 ac-idle 改为 battery；充电中能做到才以 charging 为前缀额外执行，不改电池优化设置：

~~~bash
pmset -g batt > ac-idle-telemetry-power-before.txt
mac/build/metrics_selftest --telemetry 150 > ac-idle-telemetry.txt 2>&1
pmset -g batt > ac-idle-telemetry-power-after.txt
~~~

每组约 2.5 分钟，遥测诊断不启动网络探测。列出 SystemLoad 每次变化的 t_s、相邻变化间隔、有效间隔数量及观测更新周期；不足两次变化则写观察不足，不猜周期。保留原始累加值/计数及差分平均值。SystemLoad 仍未加入 Snapshot。

## 6. 活动监视器同步（能读屏幕才做，否则未测）

~~~bash
mac/build/metrics_selftest --mem > mem.txt 2>&1
mac/build/metrics_selftest --samples 10 "$probe_flag" > "top3-${probe_tag}.txt" 2>&1
~~~

--mem 每 2 秒输出一块共 5 次，每次同步记录活动监视器“内存”页的已使用 / App / 联动 / 被压缩 / 已缓存文件 / 交换六个数和时间（可截图），注明显示单位，Snapshot 仍为 A 口径。--samples 10 期间切到“所有进程”，记录同时刻前三名及百分比；活动监视器数值先除以 10（或实际逻辑核数）再比较。不要修改已验证的进程单位校准。

## 7. 交回

交回全部 stdout/stderr、report.md（每项通过 / 偏差 / 未测，附数值、供电状态和探测口径）、build.log、build-fixed.log/fix.patch（如有）及同步截图。区分编译通过、读数验证和性能验收。暂停/恢复、模式切换及缺硬件环境的降级路径无可执行入口或条件时记未测。保留旧 TESTING_phase0.md / TESTING_phase0_1.md，不重跑其整套流程。

**输出可能含硬件序列号和网络地址，只交给开发者，不公开上传。**
