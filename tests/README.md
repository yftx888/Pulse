# 定向测试

这些程序不进入 `build.bat`，也不会启动 Pulse 界面。

## 安装参数

在仓库根目录用 Windows PowerShell 5.1 运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\install_params_selftest.ps1
```

脚本只解析 `scripts\install.ps1`，并在子进程里跑真实的参数块、主流程、`Fail` 和 `Read-YesNo`。停止进程、复制文件、建目录、快捷方式、开机自启和 `Start-Process` 都被替换。它不应创建 `C:\PulseTestIsolated`，也不应写快捷方式或注册表。用例包含交互 Y/Y、Y/N、N/Y、N/N、本地化回答、各开关组合及 Quiet；结果消息从真实 `IfStatementAst.ElseClause` 和 `Write-Host` 参数提取。

## 电源分类与界面模型

在已能编译本仓库的 MinGW-w64 g++ 13.2 环境中，于仓库根目录运行：

```bat
g++ -std=c++17 -O2 -static -finput-charset=UTF-8 -fexec-charset=UTF-8 -Isrc -o tests\feedback_selftest.exe tests\feedback_selftest.cpp src\ui_model.cpp src\i18n.cpp -luser32
tests\feedback_selftest.exe
```

测试直接调用 `ClassifyBattery`、`AcDischargeDebounce`、`UseSystemBatteryEta`、`NeedsPackagePower`、`NextCollectionTrace`、`CollectionWants` 和 `Model::Update`，以及正常采样/手动诊断复用的 `ResolveMeterStatus`、`InterpretMeterDomain`。覆盖 Power-only、数组读取失败、坏 CStatus、NaN/Inf、整数 Energy 差分、预热/不变/重置、中英文系统 ETA 与迷你/任务栏显示、离线 AC + 零 Rate，以及采集轨迹在 Unknown 上保持上次确认来源的序列。新增覆盖 AC 在线时无效速率或低于 1 W 按 AcIdle 分类、恰好 1 W 的边界、10 秒进入/5 秒退出防抖（含 9.9/10 秒、4.9/5 秒、抖动重置、hard_exit、Reset、0.5/2 秒混合采样与毫秒计时零点）、平衡计划的非平衡 overlay、其他计划优先及查询失败的中英文 scheme 文案；检查中性「插电放电 / AC + battery」和硬件卡片 96 DIP。不要加 `-municode`；命令中的 `-Isrc` 是必需的。

这些测试不模拟 DirectWrite 排版或真实 PDH/电池驱动。窗口宽度、换行、诊断文件写入及实际硬件状态需要启动 Pulse 实测。
