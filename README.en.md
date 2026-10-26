# Pulse

**English** | [简体中文](README.md)

Pulse is a floating desktop widget for Windows that can also live in the taskbar. A single statically linked `Pulse.exe`, built with native C++17, Win32, and Direct2D, displays hardware, battery, and network status.

[Download the latest release](https://github.com/yftx888/Pulse/releases/latest) · Windows 10 / 11, 64-bit · No installation required

<p align="center"><img src="docs/screenshots/demo-full.webp" alt="Pulse full mode" width="400"></p>
<p align="center"><img src="docs/screenshots/demo-compact.webp" alt="Pulse mini mode" width="760"></p>

## Features

By default, Full mode shows the following from top to bottom:

- CPU / GPU / Memory / Processor: usage for CPU, the GPU 3D engine, and memory. Processor, CPU, and memory power come from Energy Meter package, pp0, and DRAM. When an NVIDIA discrete GPU is detected and its system-reported device power state is D0, the GPU tile shows its power via NVML bundled with the graphics driver, without administrator privileges. A sleeping NVIDIA GPU shows "dGPU idle"; Pulse makes no NVML calls while it sleeps. AMD discrete GPU power is not supported yet. With no NVIDIA GPU or an unavailable NVML reading, the tile keeps processor graphics-domain (pp1) power. pp1 is not discrete-GPU board power, and GPU usage may refer to another adapter. A missing counter, no matching instance, warmup, or a failed read shows "--" with a short reason, never a fake 0 W.
- Battery: Full mode shows the Control Panel power plan's actual name ("Power plan · …"). With the system Balanced plan and a non-balanced Windows power mode, it shows "Power mode · Best performance / Best efficiency / Longer battery life" instead. Custom system plan names are left as-is. Battery saver is a short tag beside the name and does not replace it. Charge or discharge watts, smoothed Time left or Full in, To 20%, and the average since unplugging are calculated only for confirmed on-battery discharge or normal charging. External power with the battery still discharging is labeled "AC + battery"; the battery card shows net discharge and does not guess that the adapter is faulty. Discharge below 1 W or lasting less than 10 seconds is shown as connected to AC power. Once confirmed, discharge stays indicated through brief rate drops and exits after 5 seconds below the threshold; unplugging, charging, or a failed read exits immediately. If Pulse starts during confirmed on-battery discharge, the average is measured from startup.
- Network: upload and download speeds for physical network adapters only, with prominent HTTP Latency and Offline status.
- Top processes by CPU usage, excluding Pulse itself. Pulse's own usage is shown separately.

Mini mode is a single line that can show Battery %, Time left or Full in, a charging lightning indicator, and an external-power indicator. Power and the Full-mode chart use the label System only when external power is known to be offline and the battery is discharging. While plugged in, including external power with the battery still discharging, they stay on processor package power (CPU pkg in Mini mode, Processor in Full mode). Unknown external power is labeled Battery or unknown, never System. Switching sources breaks the chart so package and battery power are not connected. The fourth hardware tile is always labeled Processor.

Taskbar mode embeds two rows of data in the taskbar, so it takes no desktop space. By default it shows Network speed, Time left / Full in, Power, CPU, Memory, and Latency:

- By default (Automatic), it sits on the right side of the taskbar next to the clock. On Windows 11 with centered taskbar icons, when many open apps would put icons under it, it moves to the left of the Start button and leaves room for Widgets on the left. If neither side has room, it stays where it is instead of jumping back and forth. Taskbar position in the right-click menu can fix it to the left or right.
- Default text colors follow the light or dark taskbar. The battery estimate shows time left while discharging and time to full while charging.
- Left-click shows or hides the Full mode panel; right-click opens the menu. It re-embeds itself automatically after Explorer restarts.
- Vertical or side taskbars are not supported yet. If embedding fails, Pulse temporarily shows the Mini mode window and retries about every 30 seconds. The Windows 10 taskbar has not been tested.

Right-click the window, the taskbar data, or the tray icon and open Show items to choose components separately for each mode:

| Mode | Available components |
| --- | --- |
| Full mode | CPU / GPU / Memory / Processor, Battery, Power chart, Network, Top processes, Disk speed, System drive usage, System info, GPU (memory and temperature), Wi-Fi, Battery health, FPS |
| Mini mode | Battery %, Time left / Full in, Power, CPU %, Memory %, GPU %, GPU temp, Network speed, Latency, Disk, CPU frequency, FPS |
| Taskbar mode | Same as Mini mode |

Optional metrics are collected only while their components are enabled; disabling them stops collection. Basic sampling for CPU, memory, battery, and network speed remains active. Network probes run while Disconnect alerts are enabled or the interface shows latency, and stop when neither needs them. System info includes CPU frequency, Memory used / total, Processes, and Uptime.

Other features:

- Foreground app FPS, measured through ETW DXGI/D3D9 Present events, requires administrator privileges. If FPS is enabled without sufficient privileges, the menu offers Restart as administrator (for FPS). Some apps may not expose a readable frame rate.
- GPU temperature uses the same data source as Task Manager and needs no administrator privileges; with several GPUs, the hottest one is shown. It is usually available on discrete GPUs (NVIDIA / AMD) and AMD integrated graphics; Intel integrated graphics that do not report a temperature show none. It does not wake a sleeping discrete GPU. CPU temperature would require a kernel driver and is not provided.
- Full mode refreshes every 0.5 / 1 / 2 seconds, with a default of 1 second. Mini and Taskbar modes offer 1 / 2 / 5 seconds, with a default of 2 seconds. Opening the taskbar details panel uses the Full mode interval.
- Mini window opacity offers 100% (default) / 90% / 80% / 70% / 60%. All three modes support Select all and Select none. Mini and Taskbar modes each have independent value text colors and component ordering, while warning colors are preserved.
- Slow latency threshold can be set to 50 / 100 / 150 / 200 / 300 / 500 / 800 ms, with a default of 500 ms.
- Network disconnected and Network restored cards fade out after about 8 seconds. Hovering pauses the timer; moving away starts a roughly 3-second timer before they fade out. Disconnect alerts and Alert sound have separate switches. Sleep does not count as an outage.
- Size from 80% to 200%, also adjustable with Ctrl + mouse wheel.
- Interface language: English or Chinese, switchable from the right-click menu (语言 / Language), follows the Windows display language by default.
- Dark context menus, a tray icon, Always on top, Start with Windows, and Reset to defaults for component selections.

## Screenshots

Full mode:

![Pulse full mode](docs/screenshots/native-full.png)

Mini mode:

![Pulse mini mode](docs/screenshots/native-compact.png)

Taskbar mode (light taskbar, Chinese interface, charging):

![Pulse taskbar mode](docs/screenshots/native-taskbar.png)

## System requirements

- Windows 10 or Windows 11, 64-bit.
- Runs as a standard user. Only FPS requires administrator privileges.
- Hardware power comes from Energy Meter (RAPL) performance counters, without administrator privileges. Full mode prefers differences between `\Energy Meter(*)\Energy` samples (raw unit pWh; watts = delta × 3.6e-9 / seconds) and falls back to `\Energy Meter(*)\Power` (mW / 1000). Mini mode reads Power directly. With the hardware tiles enabled in Full mode, an NVIDIA GPU uses NVML only when the system reports device power state D0. Mini and Taskbar modes do not collect discrete-GPU power or load NVML. AMD ADLX is not supported, and D3DKMT adapter Power (0.1 percent) is never treated as watts.
- If those counters are missing, no package / pp0 / pp1 / DRAM instance matches, or a read fails, the value stays unknown (`--`) with a short reason. One failure or a warmup is not treated as permanent absence. Memory power requires a DRAM instance. Battery charge and discharge power comes from the Windows battery interface; an unknown rate is not shown as 0 W.

## Installation

Download and extract the zip, then run `scripts\install.cmd`. The script:

1. Exits Pulse if it is running.
2. Copies `Pulse.exe` to `%LOCALAPPDATA%\Pulse`.
3. Creates a Start menu shortcut for the current user.
4. Uses the options below to determine whether to create a desktop shortcut and enable startup with Windows.
5. Starts the installed copy of Pulse.

Installation and normal use do not require administrator privileges; FPS does. You can also run `Pulse.exe` directly after extraction. Settings are still saved in `%LOCALAPPDATA%\Pulse`.

`install.ps1` looks for `Pulse.exe` in the script's directory and its parent. When installing from this repository, build `Pulse.exe` in the repository root first.

| Option | Effect |
| --- | --- |
| (No options) | Asks whether to create a desktop shortcut and start with Windows |
| `-Desktop` | Creates a desktop shortcut without prompting |
| `-Autostart` | Enables startup with Windows without prompting |
| `-Quiet` | Suppresses prompts; options not explicitly specified are not enabled |

Examples:

```bat
scripts\install.cmd
scripts\install.cmd -Desktop -Autostart
scripts\install.cmd -Quiet
```

Startup uses the current user's `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` key, with a value named `Pulse` containing the full, quoted path to `Pulse.exe`. This is the same entry controlled by Start with Windows in the app menu. The menu writes the path of the currently running copy; the installer writes `%LOCALAPPDATA%\Pulse\Pulse.exe`.

## Uninstallation

Run `scripts\uninstall.cmd`. The script exits only Pulse processes identified as this app by window class or version information, skipping other programs with the same name. It removes the Run value described above and removes Start menu and desktop `Pulse.lnk` shortcuts that target `Pulse.exe` or have no target.

Window position, mode, Always on top, component selections, component order and text colors, Mini opacity, Taskbar position, both refresh intervals, Size, language (`[Window] language=auto|zh|en`), Slow latency threshold, Disconnect alerts, and Alert sound are saved in `%LOCALAPPDATA%\Pulse\settings.ini`. Logs for network probes, frame-rate diagnostics, and other events are stored in `%LOCALAPPDATA%\Pulse\pulse_net.log`, in the same folder as the installed app. When the log exceeds 1 MB, it is renamed to `pulse_net.log.1` and a new log is started. No settings are written to `%APPDATA%` (the roaming directory), but the uninstaller still checks it for settings and logs with the same names.

Refresh intervals use `[Window] refresh_ms=500|1000|2000` (default 1000) and `mini_refresh_ms=1000|2000|5000` (default 2000). Mini opacity uses `[Compact] opacity=100|90|80|70|60` (default 100). `[Compact]` and `[Taskbar]` each store `order` (comma-separated component keys) and `color_<key>=#RRGGBB` (for example, `color_cpu=#38BDF8`). Unknown or duplicate order keys are ignored; missing components are appended in the mode's default order. Missing or invalid colors use the defaults, and Reset colors deletes the corresponding color keys.

If these files exist, the script asks whether to remove them. Choosing Y deletes the entire `%LOCALAPPDATA%\Pulse` directory and any detected settings and log files in the roaming directory. Choosing N removes only `Pulse.exe` and the shortcuts, keeping settings and logs. If no settings or logs are found, the installation directory is removed by default.

| Option | Effect |
| --- | --- |
| (No options) | Asks whether to delete settings and logs if they exist |
| `-KeepConfig` | Keeps settings and logs without prompting |
| `-RemoveConfig` | Deletes settings, logs, and the installation directory without prompting |

## Usage

- Drag an empty area of the window to move it. Its position is saved.
- Double-click an empty area, or click the mode button, to switch between Full and Mini modes.
- The pin button toggles Always on top, which is enabled by default.
- Minimizing pauses sampling for the UI. Left-click the tray icon to hide or show the window. Hiding also pauses UI sampling, while network probes continue if Disconnect alerts are enabled. Sampling resumes when the window is shown again.
- In Taskbar mode, left-click the taskbar data or the tray icon to show or hide the Full mode panel.
- The close button exits the app.
- Hold Ctrl and scroll inside the window to change the scale by 10 percentage points per notch, between 80% and 200%. Use the mouse wheel without Ctrl to scroll content that extends beyond the screen.

The window, the taskbar data, and the tray icon share this context menu:

| Menu item | Effect |
| --- | --- |
| Full mode / Mini mode / Taskbar mode | Switches display mode |
| Taskbar position | Automatic (default) / Right / Left. Right is next to the clock; Left is to the left of the Start button. Right and Left stay fixed |
| Size | 80% / 100% (default) / 125% / 150% / 175% / 200% |
| Mini opacity | 100% (default) / 90% / 80% / 70% / 60%; affects only the floating Mini window, including fallback when taskbar embedding fails |
| 语言 / Language | Follow system (default), 中文, or English |
| Refresh interval | Two independent groups: Full mode at 0.5 / 1 (default) / 2 seconds; Mini / Taskbar at 1 / 2 (default) / 5 seconds |
| Slow latency threshold | 50 / 100 / 150 / 200 / 300 / 500 (default) / 800 ms |
| Disconnect alerts | Toggles Network disconnected and Network restored cards; enabled by default |
| Alert sound | Toggles sound separately; enabled by default and unavailable when Disconnect alerts are disabled |
| Show items → Full mode / Mini mode / Taskbar mode | Selects components for each mode, with Select all (including FPS) and Select none |
| Show items → Mini mode / Taskbar mode → Reorder | Moves enabled components up, down, to first, or to last; the component checklist follows the same order |
| Show items → Mini mode / Taskbar mode → Text color | Sets value text colors for enabled components separately per mode, preserving labels and warnings; Reset colors affects only that mode |
| Show items → Reset to defaults | Resets selections for all three modes and Mini / Taskbar order; keeps colors and opacity |
| Always on top | Toggles pinning the window above others |
| Start with Windows | Toggles startup for the current user |
| Power diagnostics | Starts a separate process that samples for about one second, writes the plan, signed battery rate, and Energy Meter instances to `%LOCALAPPDATA%\Pulse\power-diagnostics.txt`, and opens it. Nothing is uploaded and settings are not changed |
| Open log | Opens the local log |
| Restart as administrator (for FPS) | Appears when FPS is enabled in the current mode and administrator privileges are missing |
| Exit | Exits the app |

## Network latency

Latency is measured with WinHTTP HTTP requests through the system proxy, rather than ICMP ping. In some proxy or TUN configurations, ping results may not reflect actual access latency. The probe addresses are:

- `http://www.msftconnecttest.com/connecttest.txt`
- `http://www.gstatic.com/generate_204`
- `http://connect.rom.miui.com/generate_204`

Under normal conditions, Pulse probes the first address about every 5 seconds. If it fails, Pulse tries the two fallback addresses in parallel. After an unavailable result or a network-adapter change, it probes all three in parallel and uses the first qualifying response headers received. Only HTTP 2xx status codes count as success; redirects (such as login pages) and error codes count as failures for that probe attempt. Response bodies are still not checked, and redirects are not followed. Latency is timed from the start of the current probe stage; fallback latency excludes the time spent waiting for the primary address to fail.

| Result | Interpretation |
| --- | --- |
| Request succeeds with latency < selected threshold | Connected |
| Latency ≥ selected threshold (default 500 ms) and < 1000 ms | Slow |
| Probe round fails, or latency ≥ 1000 ms | Counts as one unavailable result |
| 3 consecutive unavailable results | Connection is marked Offline |
| One successful request with latency < 1000 ms | Clears the failure count; restores an Offline connection |
| All physical network adapters are disconnected, confirmed after a short debounce | Connection is marked Offline immediately, without waiting for 3 unavailable HTTP results |

After an unavailable result, Pulse checks about every 1 second. Once Offline, it retries about every 1.5 seconds for the first 2 minutes, then about every 3 seconds. While all physical adapters are disconnected, Pulse sends no requests; it probes as soon as adapter status changes, with a fallback check every 15 seconds. After physical adapters disconnect, the physical connection must return before Pulse can mark the connection as restored, even if a local proxy returns a successful response. System sleep pauses probing and clears outage timing. Probing resumes about 3 seconds after wake.

When Disconnect alerts are off and the current interface shows no latency (for example, Mini or Taskbar mode without Latency selected), network probing stops completely and sends no requests.

## Building from source

Install 64-bit MinGW-w64 g++ 13.2, including `windres`, and add it to PATH. From the repository root, run:

```bat
build.bat
```

In PowerShell:

```powershell
& .\build.bat
```

The script produces the statically linked `Pulse.exe` and the console self-test program `metrics_selftest.exe`. The source uses C++17 and can also be built with MSVC: run `build_msvc.bat` from an x64 Developer Command Prompt. Its output goes to `build-msvc\`. Release builds use MinGW.

To package a zip (runs `build.bat` first):

```bat
scripts\package.cmd
scripts\package.cmd -NoBuild
scripts\package.cmd -Version 1.2.1
```

`-NoBuild` skips compilation only if `Pulse.exe` already exists in the repository root. Without `-Version`, the script reads `FileVersion` from `app.rc`, falling back to `0.1.0` if it cannot be read. The output is `dist\Pulse-<version>-win-x64.zip`, containing `Pulse.exe`, `README.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, and the installation and uninstallation scripts from `scripts\`.

GitHub Actions automatically builds and publishes a Release when a `v*` tag is pushed.

## Resource usage

The following figures were measured on an Intel Core Ultra 5 226V laptop. CPU usage uses Pulse's process CPU time, expressed as a percentage of one CPU core; memory is the Memory column in Task Manager (private working set):

| Mode | CPU usage | Memory |
| --- | --- | --- |
| Taskbar mode | About 0.1%–0.15% of one core | About 13 MB |
| Mini mode | About 0.1%–0.3% of one core | About 13 MB |
| Full mode | Runs the full collection path on each refresh; about 0.5%–1.5% of one core | About 40 MB |

Mini mode, Taskbar mode, and the network alert cards use software rendering and do not load the graphics driver. Full mode uses hardware rendering, and the graphics driver uses more memory. After the Full mode panel has been opened from Mini or Taskbar mode, the driver stays loaded and memory does not return to its initial level until Pulse restarts.

Optional metrics still depend on component selections and their individual collection intervals. These figures vary with hardware, enabled components, refresh interval, and background load, and are for reference only.

## Acknowledgments

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer and privacy

- This software is provided "as is", without warranty. You use it at your own risk, and the author is not liable for losses caused by its use. See [LICENSE](LICENSE) (MIT) for the governing terms.
- Pulse reads system performance counters, battery information, processes, and network status. It changes system settings only when you choose Start with Windows. In a normal environment, runtime settings and logs are written only to `%LOCALAPPDATA%\Pulse`. Running Power diagnostics also writes `power-diagnostics.txt`; it is not uploaded. Toggling Start with Windows writes or deletes the `Pulse` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. The installer also copies the app and creates the shortcuts described above.
- Network access is used only for latency measurement, by periodically requesting the three probe addresses listed above through the system proxy. Pulse does not collect or upload personal data over the network and has no analytics, telemetry, or automatic updates.
- Logs are stored locally and are never uploaded.
- Administrator privileges are used only for the FPS feature's ETW session. They are unnecessary if you do not need FPS.
- `Pulse.exe` is not code-signed, so Windows SmartScreen may warn on first launch. You can select More info → Run anyway. Download only from this project's GitHub Releases.

## License

[MIT License](LICENSE). Copyright © 2026 yftx.
