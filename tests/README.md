# 定向测试

这些程序不进入 `build.bat`，也不会启动 Pulse 界面。

## 安装参数

在仓库根目录用 Windows PowerShell 5.1 运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\install_params_selftest.ps1
```

脚本只解析 `scripts\install.ps1`，并在子进程里跑真实的参数块、主流程、`Fail` 和 `Read-YesNo`。停止进程、复制文件、建目录、快捷方式、开机自启和 `Start-Process` 都被替换。它不应创建 `C:\PulseTestIsolated`，也不应写快捷方式或注册表。用例包含交互 Y/Y、Y/N、N/Y、N/N、本地化回答、各开关组合及 Quiet；结果消息从真实 `IfStatementAst.ElseClause` 和 `Write-Host` 参数提取。

## 卸载参数

在仓库根目录用 Windows PowerShell 5.1 运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\uninstall_params_selftest.ps1
```

脚本解析真实的 `scripts\uninstall.ps1`，在子进程里执行参数块、主流程、`Fail`、`Read-YesNo`、`Get-PulseDataFiles` 和 `Remove-EmptyDirectory`。以临时目录模拟本地和漫游 AppData，删除、结束进程、自启和快捷方式操作全部替换为记录；删除后的目录枚举使用虚拟删除记录，实际测试文件必须仍然存在。只在测试结束时清理经过路径校验的临时测试目录，不触碰真实安装或配置。

覆盖无数据时默认 / `-RemoveConfig` / `-KeepConfig`、本地 settings.ini、本地与漫游日志、仅轮转日志、仅功耗诊断文件（包含漫游目录）和交互 Y/N。检查返回 Count、元素类型、文件清单、计划删除路径及递归标记、询问次数和成功退出。另把真实函数的返回值恢复为旧的 `return ,$result`，要求无数据 + `-RemoveConfig` 子进程出现嵌套数组和 StartsWith 错误，作为负对照。

## CI 共用入口

MinGW-w64 已加入 PATH 时，在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_ci_tests.ps1
```

该入口按下文命令编译并运行 feedback_selftest，再用 Windows PowerShell 5.1 分别运行安装和卸载参数测试；任一编译或测试失败即返回非零。build.yml 在 MinGW 编译后、上传前调用，release.yml 在编译后、打包前调用。CI 不运行字体相关的 dgpu_layout_selftest、真实硬件 metrics_selftest 或 GUI。

## 电源分类与界面模型

在已能编译本仓库的 MinGW-w64 g++ 13.2 环境中，于仓库根目录运行：

```bat
g++ -std=c++17 -O2 -static -finput-charset=UTF-8 -fexec-charset=UTF-8 -Isrc -o tests\feedback_selftest.exe tests\feedback_selftest.cpp src\ui_model.cpp src\i18n.cpp -luser32
tests\feedback_selftest.exe
```

测试还覆盖无 Win32 依赖的 `ProbeHttpStatusOk`：200/201/204/206/299 成功，199/300 等边界、登录页重定向、407、500/502、0 和 ULONG_MAX 失败。

测试直接调用 `ClassifyBattery`、`AcDischargeDebounce`、`UseSystemBatteryEta`、`NeedsPackagePower`、`NextCollectionTrace`、`CollectionWants` 和 `Model::Update`，以及正常采样/手动诊断复用的 `ResolveMeterStatus`、`InterpretMeterDomain`。覆盖 Power-only、数组读取失败、坏 CStatus、NaN/Inf、整数 Energy 差分、预热/不变/重置、中英文系统 ETA 与迷你/任务栏显示、离线 AC + 零 Rate，以及采集轨迹在 Unknown 上保持上次确认来源的序列。新增覆盖 AC 在线时无效速率或低于 1 W 按 AcIdle 分类、恰好 1 W 的边界、10 秒进入/5 秒退出防抖（含 9.9/10 秒、4.9/5 秒、抖动重置、hard_exit、Reset、0.5/2 秒混合采样与毫秒计时零点）、平衡计划的非平衡 overlay、其他计划优先及查询失败的中英文 scheme 文案；检查中性「插电放电 / AC + battery」和硬件卡片 96 DIP。不要加 `-municode`；命令中的 `-Isrc` 是必需的。

这些测试不模拟 DirectWrite 排版或真实 PDH/电池驱动。窗口宽度、换行、诊断文件写入及实际硬件状态需要启动 Pulse 实测。

## NVIDIA 独显功耗

上面的同一条 feedback_selftest 编译/运行命令同时覆盖无 Win32 依赖的 src/dgpu_state.h：无 NVIDIA、D0/D1/D2/D3/Unspecified 与属性读取失败、未醒时 NVML 动作为 None、PCI bus/device 匹配、NVML 错误码、功率限值及 4.9/5 秒野值缓存边界、多卡仅统计已醒设备、重复 PCI 去重、不发布失败的部分功率；并验证中英文 GPU 小字及完整/迷你/任务栏条的 CollectionWants。沿用现有 WattSuffix 的空格（例如「独显 45 W」）。

硬件诊断（先用 build.bat 构建）：

```powershell
.\metrics_selftest.exe --gpu
.\metrics_selftest.exe --power
# 显式手动诊断；可能唤醒独显，会在调用 NVML 前显示 WARNING。
.\metrics_selftest.exe --nvml
# 正常采样（仅完整模式请求 NVML，仍须先通过 CfgMgr D0 gate）
.\metrics_selftest.exe --no-probe --samples 6
.\metrics_selftest.exe --no-probe --compact --samples 6
```

--gpu 不调用 NVML；--power 仅在系统设备电源状态为 D0 时调用 NVML。自动测试不验证驱动加载、D3 休眠、0.5/1/2 秒采样的实际功耗或 DirectWrite 布局。需要在 NVIDIA 笔记本上实测：启动时独显为 D3 则不加载 nvml.dll、游戏唤醒后读到瓦数、退出游戏后回到休眠且保留会话但不再调用 NVML、关闭 Hardware/切换迷你/任务栏条后停止采集、睡眠恢复后重新判定。多卡及 NVML 是否拖住独显也必须用真实硬件检查。

DirectWrite 窗口宽度单测（不创建窗口或 D3D 设备）检查中文默认 320 DIP、硬件卡片 96 DIP，并打印英文宽度及相对 320 DIP 的差值；同时检查中英文 Sleeping/Glitch 小字：

```bat
g++ -std=c++17 -O2 -static -finput-charset=UTF-8 -fexec-charset=UTF-8 -Isrc -o tests\dgpu_layout_selftest.exe tests\dgpu_layout_selftest.cpp src\ui_renderer.cpp src\ui_model.cpp src\i18n.cpp -ld2d1 -ldwrite -ldwmapi -luser32 -lgdi32
tests\dgpu_layout_selftest.exe
```
