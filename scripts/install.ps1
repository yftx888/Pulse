#Requires -Version 5.1
<#
.SYNOPSIS
    把 Pulse.exe 安装到当前用户的 %LOCALAPPDATA%\Pulse。
.DESCRIPTION
    不需要管理员。无参数时询问是否创建桌面快捷方式和开机自启。
    -Quiet 时不询问。开始菜单快捷方式总会创建。
#>
[CmdletBinding()]
param(
    [switch]$Desktop,
    [switch]$Autostart,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw $Message
}

function Read-YesNo([string]$Prompt) {
    while ($true) {
        $answer = Read-Host ($Prompt + ' (Y/N)')
        if ($null -eq $answer) {
            Fail '无法读取输入。请在终端中运行，或改用 -Desktop / -Autostart / -Quiet。'
        }
        $answer = $answer.Trim()
        if ($answer -match '^(Y|y|是)$') { return $true }
        if ($answer -match '^(N|n|否)$') { return $false }
        Write-Host '请输入 Y 或 N。'
    }
}

function Find-PulseExe {
    if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        Fail '无法确定脚本所在目录。请用 -File 运行此脚本。'
    }
    $parent = Split-Path -Parent $PSScriptRoot
    $same = Join-Path $PSScriptRoot 'Pulse.exe'
    $up = Join-Path $parent 'Pulse.exe'
    $folderName = Split-Path -Leaf $PSScriptRoot
    if ($folderName -eq 'scripts' -and (Test-Path -LiteralPath $up -PathType Leaf)) {
        return [System.IO.Path]::GetFullPath($up)
    }
    if (Test-Path -LiteralPath $same -PathType Leaf) {
        return [System.IO.Path]::GetFullPath($same)
    }
    if (Test-Path -LiteralPath $up -PathType Leaf) {
        return [System.IO.Path]::GetFullPath($up)
    }
    Fail '找不到 Pulse.exe。请先在仓库根目录运行 build.bat，或把本脚本放在 Pulse.exe 同级目录，或放在它的 scripts 子目录中。'
}

function Stop-Pulse {
    $procs = @(Get-Process -Name 'Pulse' -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) {
        Write-Host 'Pulse 未在运行。'
        return
    }

    if (-not ('PulseWin32' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class PulseWin32
{
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int GetClassNameW(IntPtr hwnd, StringBuilder className, int maxCount);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);

    public static IntPtr[] GetMainWindows(uint processId)
    {
        var windows = new List<IntPtr>();
        // EnumWindows also visits hidden top-level windows (minimized to the tray).
        EnumWindowsProc callback = delegate(IntPtr hwnd, IntPtr lParam)
        {
            uint ownerId;
            GetWindowThreadProcessId(hwnd, out ownerId);
            if (ownerId == processId)
            {
                var className = new StringBuilder(256);
                if (GetClassNameW(hwnd, className, className.Capacity) > 0 &&
                    className.ToString() == "Pulse.Native.Main.v1")
                {
                    windows.Add(hwnd);
                }
            }
            return true;
        };
        if (!EnumWindows(callback, IntPtr.Zero))
        {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        return windows.ToArray();
    }
}
'@
    }

    $targets = @()
    foreach ($proc in $procs) {
        $windows = @([PulseWin32]::GetMainWindows([uint32]$proc.Id))
        $path = $null
        try { $path = $proc.Path } catch { }
        $isPulse = $windows.Count -gt 0
        if ((-not $isPulse) -and (-not [string]::IsNullOrWhiteSpace($path))) {
            try {
                $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
                $isPulse = ($version.ProductName -eq 'Pulse' -and
                    $version.FileDescription -eq 'Pulse native desktop widget')
            } catch { }
        }
        if ($isPulse) {
            $targets += [pscustomobject]@{ Process = $proc; Windows = $windows }
        } else {
            $location = '路径不可读'
            if (-not [string]::IsNullOrWhiteSpace($path)) { $location = '路径 ' + $path }
            Write-Host ("检测到另一个名为 Pulse 的程序（PID {0}，{1}），不是本程序，已跳过。" -f $proc.Id, $location)
        }
    }
    if ($targets.Count -eq 0) {
        Write-Host 'Pulse 未在运行。'
        return
    }

    # 后续只检查已识别的 PID，不再按进程名查找。
    $targetIds = @($targets | ForEach-Object { $_.Process.Id })
    Write-Host '正在退出正在运行的 Pulse...'
    foreach ($target in $targets) {
        if ($target.Windows.Count -gt 0) {
            foreach ($window in $target.Windows) {
                [void][PulseWin32]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            }
        } else {
            try { [void]$target.Process.CloseMainWindow() } catch { }
        }
    }
    $grace = (Get-Date).AddSeconds(5)
    do {
        $left = @(Get-Process -Id $targetIds -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { break }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $grace)

    $left = @(Get-Process -Id $targetIds -ErrorAction SilentlyContinue)
    foreach ($proc in $left) {
        try {
            Stop-Process -Id $proc.Id -Force -ErrorAction Stop
        } catch {
            $failure = $_
            # 进程可能恰好自行退出，只有仍在运行时才报告权限错误。
            if (-not (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) { continue }
            $accessDenied = $failure.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::PermissionDenied
            $exception = $failure.Exception
            while ($null -ne $exception) {
                if (($exception -is [System.UnauthorizedAccessException]) -or
                    (($exception -is [System.ComponentModel.Win32Exception]) -and $exception.NativeErrorCode -eq 5)) {
                    $accessDenied = $true
                    break
                }
                $exception = $exception.InnerException
            }
            if ($accessDenied) {
                Fail 'Pulse 正以管理员身份运行，脚本无法结束它。请从托盘菜单选择“退出”后重试。'
            }
        }
    }

    $deadline = (Get-Date).AddSeconds(8)
    do {
        $left = @(Get-Process -Id $targetIds -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) {
            Start-Sleep -Milliseconds 400
            return
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    if (@(Get-Process -Id $targetIds -ErrorAction SilentlyContinue).Count -eq 0) { return }
    Fail '无法结束正在运行的 Pulse。请从托盘菜单选择“退出”后重试。'
}

function New-PulseShortcut([string]$ShortcutPath, [string]$Target) {
    $directory = Split-Path -Parent $ShortcutPath
    if ([string]::IsNullOrWhiteSpace($directory)) {
        Fail "快捷方式路径无效：`"$ShortcutPath`""
    }
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    try {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut($ShortcutPath)
        $link.TargetPath = $Target
        $link.WorkingDirectory = Split-Path -Parent $Target
        $link.WindowStyle = 1
        $link.Description = 'Pulse'
        $link.IconLocation = ($Target + ',0')
        $link.Save()
    } catch {
        Fail ("无法创建快捷方式 `"{0}`"：{1}" -f $ShortcutPath, $_.Exception.Message)
    }
}

function Set-PulseAutostart([string]$Target) {
    $keyPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath, $true)
    if ($null -eq $key) {
        Fail '无法打开当前用户的开机自启注册表项 HKCU\Software\Microsoft\Windows\CurrentVersion\Run。'
    }
    try {
        $quoted = '"' + $Target + '"'
        $key.SetValue('Pulse', $quoted, [Microsoft.Win32.RegistryValueKind]::String)
    } catch {
        Fail ("无法写入开机自启：{0}" -f $_.Exception.Message)
    } finally {
        $key.Close()
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        Fail '环境变量 LOCALAPPDATA 未设置，无法确定安装目录。'
    }

    $source = Find-PulseExe
    Write-Host ("使用程序：`"{0}`"" -f $source)

    $localRoot = [System.IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\')
    $destDir = [System.IO.Path]::GetFullPath((Join-Path $localRoot 'Pulse'))
    $dest = [System.IO.Path]::GetFullPath((Join-Path $destDir 'Pulse.exe'))
    $destParent = [System.IO.Path]::GetFullPath((Split-Path -Parent $destDir)).TrimEnd('\')
    if (-not [string]::Equals($destParent, $localRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail ("安装目录不在 LOCALAPPDATA 下：`"{0}`"" -f $destDir)
    }
    if (Test-Path -LiteralPath $destDir -PathType Leaf) {
        Fail ("安装路径被同名文件占用：`"{0}`"" -f $destDir)
    }

    Stop-Pulse

    if (-not (Test-Path -LiteralPath $destDir)) {
        try {
            New-Item -ItemType Directory -Path $destDir -Force | Out-Null
        } catch {
            Fail ("无法创建安装目录 `"{0}`"：{1}" -f $destDir, $_.Exception.Message)
        }
    }
    $sameFile = [string]::Equals($source, $dest, [System.StringComparison]::OrdinalIgnoreCase)
    if (-not $sameFile) {
        if (Test-Path -LiteralPath $dest) {
            $existing = Get-Item -LiteralPath $dest -Force
            if ($existing.IsReadOnly) { $existing.IsReadOnly = $false }
        }
        try {
            Copy-Item -LiteralPath $source -Destination $dest -Force
        } catch {
            Fail ("无法把 Pulse.exe 复制到 `"{0}`"：{1}" -f $dest, $_.Exception.Message)
        }
    }
    if (-not (Test-Path -LiteralPath $dest -PathType Leaf)) {
        Fail ("安装后找不到 `"{0}`"。" -f $dest)
    }
    Write-Host ("已安装到：`"{0}`"" -f $dest)

    $createDesktop = [bool]$Desktop
    $enableAutostart = [bool]$Autostart
    if ((-not $Quiet) -and (-not $Desktop) -and (-not $Autostart)) {
        $createDesktop = Read-YesNo '是否创建桌面快捷方式？'
        $enableAutostart = Read-YesNo '是否设置开机自启？'
    }

    $programs = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Programs)
    if ([string]::IsNullOrWhiteSpace($programs)) {
        Fail '无法确定当前用户的开始菜单程序目录。'
    }
    $startLink = Join-Path $programs 'Pulse.lnk'
    New-PulseShortcut -ShortcutPath $startLink -Target $dest
    Write-Host ("已创建开始菜单快捷方式：`"{0}`"" -f $startLink)

    if ($createDesktop) {
        # 变量名不能叫 $desktop：与 param 里的 [switch]$Desktop 同名（PowerShell 变量名不区分大小写），
        # 给 switch 参数变量赋字符串会抛类型转换异常。
        $desktopFolder = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Desktop)
        if ([string]::IsNullOrWhiteSpace($desktopFolder)) {
            Fail '无法确定桌面目录。'
        }
        $desktopLink = Join-Path $desktopFolder 'Pulse.lnk'
        New-PulseShortcut -ShortcutPath $desktopLink -Target $dest
        Write-Host ("已创建桌面快捷方式：`"{0}`"" -f $desktopLink)
    } else {
        Write-Host '未创建桌面快捷方式。'
    }

    if ($enableAutostart) {
        Set-PulseAutostart -Target $dest
        Write-Host ("已设置开机自启：`"{0}`"" -f $dest)
    } else {
        Write-Host '未设置开机自启。'
    }

    Start-Process -FilePath $dest -WorkingDirectory $destDir | Out-Null
    Write-Host '已启动 Pulse。'
} catch {
    $message = $_.Exception.Message
    if ([string]::IsNullOrWhiteSpace($message)) { $message = [string]$_ }
    Write-Host ("错误：" + $message)
    exit 1
}
exit 0
