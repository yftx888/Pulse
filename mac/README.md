# Pulse for macOS

Apple Silicon 菜单栏版，单行显示整机功耗与预计剩余使用时间，例如 `5.2W 6:30`。

- 外接电源时显示 SMC 整机负载；纯电池时显示可信电池放电功率。处理器功耗在详情中单独显示。
- 剩余时间仅在可信纯电池放电时显示，外电／充电为 `--:--`。缺失功率为 `--W`。
- 左键打开详情，Esc 或点外关闭；右键设置详情内容、刷新间隔、语言、网络提醒，并可打开日志和功耗诊断。
- 菜单栏默认 2 秒刷新，详情打开时 1 秒。1／2／5 秒设置只作用于菜单栏。

当前构建最低系统版本为 macOS 13，真机验收环境为 Apple M5 / macOS 26.6。SMC 和 IOReport 在其他机型上的读数仍需实机核对。首版为 ad-hoc 签名，未做 Apple 公证。

## 构建与验证

使用已有 Xcode Command Line Tools，在仓库根目录执行：

```sh
bash mac/build.sh
bash mac/test.sh
```

产出 `mac/build/Pulse.app`、`metrics_selftest` 和 `Pulse-mac-arm64.zip`。`test.sh` 包含读数语义和生产视图离屏绘制，需要本机 GUI 会话；CI 使用 `bash mac/test.sh --logic-only`，不将虚拟机结果作为 SMC／睡醒／真实 GUI 验收。

正常启动使用系统代理进行按需网络探测。隔离离线试运行：

```sh
test_home=$(mktemp -d)
CFFIXED_USER_HOME="$test_home" mac/build/Pulse.app/Contents/MacOS/Pulse --no-probe
```

`--require-proxy` 要求现有静态 HTTP(S) 系统代理，缺失时不发请求；它不会修改系统代理。`--no-probe` 禁用主动探测，本地网卡吞吐读取仍保留。

## 配置与日志

正式配置位于 `~/Library/Application Support/Pulse/settings.ini`，通过菜单修改后即时保存，保留未知及 Windows 专用字段。

网络日志位于 `~/Library/Logs/Pulse/pulse_net.log`，记录监测启停与断网／恢复事件，不逐次记录成功探测；超过 1 MiB 时保留一份 `.1` 归档。新建日志权限为 0600，右键“打开日志”可查看。

`--lifecycle-log <新文件路径>` 是默认关闭的验收记录选项，详见 [生命周期测试](TESTING_lifecycle.md)。开启日志的运行不用于测量正常模式 CPU 开销。

安装到 `~/Applications/Pulse.app` 与菜单“登录时打开”是独立操作，构建脚本不会安装或注册自启。设置保持、睡眠唤醒及诊断记录的复现步骤见 [生命周期验收](TESTING_lifecycle.md)。
