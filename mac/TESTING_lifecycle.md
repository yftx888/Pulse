# 设置保持与睡眠唤醒验收

使用本机 GUI 会话、已有构建及独立 `CFFIXED_USER_HOME`。不要写正式配置，不改登录项。主动网络探测统一关闭；这项验收不能替代真实断网／恢复或联网性能测试。

## 设置修改后重开

1. 在新的隔离家目录启动 `Pulse.app/Contents/MacOS/Pulse --no-probe`，保存 PID、二进制哈希和 stderr。
2. 通过真实右键菜单，将刷新间隔改为 5 秒，启用详情的功耗走势图，再改成 English；菜单 Exit 正常退出。
3. 保存生成的 settings.ini 与进程退出状态。用**同一个家目录**重新启动；不要重写配置。
4. 可加 `--snapshot-dir <新的绝对目录> --snapshot-exit`，核对 state.txt 的 `refresh_ms=5000`、`language=en`，以及 menu.txt 中 5 s／English／Power chart 的勾选；打开 details.png 检查英文图表。比较重开前后配置哈希。
5. 分开记录用户的真实菜单操作、新进程读取及离屏渲染，不把配置文件手工改写当成菜单验收。

## 可选的进程内生命周期记录

`--lifecycle-log <新的文件绝对路径>` 创建一个权限 0600 的 JSONL 文件。父目录必须已存在；文件存在时拒绝覆盖，启动失败返回非零并将错误写入 stderr。运行中的记录写入失败也会报告到 stderr，因此验收时同时检查退出码和 stderr。

默认不开启。不增加采样器、定时器、采样频率或主动网络请求。记录只来自既有 GUI 回调和既有 Snapshot，字段不含电池序列号、SSID、进程名称或网络端点。开启时每条记录会刷新到文件，因此不能拿这次运行当正常模式的 CPU 性能基准。

```sh
test_home=$(mktemp -d)
test_output=$(mktemp -d "$PWD/mac-validation-lifecycle-XXXXXX")
pmset -g log > "$test_output/pmset-before.txt"
CFFIXED_USER_HOME="$test_home" mac/build/Pulse.app/Contents/MacOS/Pulse \
  --no-probe --lifecycle-log "$test_output/events.jsonl" \
  2> "$test_output/stderr.txt"
pmset -g log > "$test_output/pmset-after.txt"
```

需要同时保留终端记录并在 Mac 上操作。复用设置验收时，将 `test_home` 指向已修改的同一隔离目录。用户准备好后先打开详情，再合盖、等待、开盖解锁；恢复后观察约 15 秒、重新打开详情并用 Esc 关闭，最后菜单 Exit。不要使用 `--open-details` 验证正常关闭行为，它会强制保持打开。

记录包括 started、sample、settings-changed、will-sleep、did-wake、screens-sleep、screens-wake、shutdown。sample_index 只随既有实际采样增长。事件只包含状态字段；只有 sample 含功率与读数，避免把唤醒事件前的旧 Snapshot 当作新数据。缺失数值在 JSON 中为 null。

判据：

- pmset 必须出现本次真实 Sleep 及 Wake／FullWake。屏幕熄灭、轮询间隙或单独一个 will-sleep 不能证明系统真正睡眠。
- 同一 PID 收到真实睡醒回调；两个 asleep 标志清除前无 sample。显示与系统唤醒的通知顺序允许不同。
- AC 的醒后 sample 恢复 system_source=smc 和有效整机读数；纯电池则检查可信电池来源及续航。不要将 AC 结果外推为纯电池测试。
- CPU／吞吐的第一拍缺失是重建差分基线的预期；打开详情后 IOReport 首拍预热、随后恢复。不要把缺失填为 0。
- 设置保持，正常交互恢复，stderr 空，菜单退出为 exit 0。关闭探测时 Checking 不算故障，也不能证明真实通知正常。

将本次系统日志、进程记录和用户实际观察一起保存在本地验收目录，分别标注已通过范围和未测场景。
