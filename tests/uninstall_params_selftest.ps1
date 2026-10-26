# Directed tests for the real uninstaller with all destructive actions mocked.
#Requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$scriptPath = Join-Path $repoRoot 'scripts\uninstall.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw 'Cannot parse scripts/uninstall.ps1.' }
$bytes = [System.IO.File]::ReadAllBytes($scriptPath)
if ($bytes.Length -lt 3 -or $bytes[0] -ne 0xEF -or $bytes[1] -ne 0xBB -or $bytes[2] -ne 0xBF) {
    throw 'scripts/uninstall.ps1 must retain its UTF-8 BOM.'
}

function Get-FunctionAst([string]$Name) {
    foreach ($node in $ast.FindAll({
        param($item)
        $item -is [System.Management.Automation.Language.FunctionDefinitionAst]
    }, $true)) {
        if ($node.Name -eq $Name) { return $node }
    }
    throw ("Missing function " + $Name)
}

$functions = @('Fail', 'Read-YesNo', 'Get-PulseDataFiles', 'Remove-EmptyDirectory')
$functionNodes = @()
foreach ($name in $functions) { $functionNodes += Get-FunctionAst $name }
$mainNodes = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [System.Management.Automation.Language.TryStatementAst]
})
if ($mainNodes.Count -ne 1 -or -not $ast.ParamBlock) { throw 'Cannot identify the uninstaller main try/parameters.' }
$mainAst = $mainNodes[0]

# Fail closed if the extracted code starts calling a new command. The real
# process, registry and COM helpers are deliberately not copied to the child.
$allowed = @('Fail', 'Read-Host', 'Write-Host', 'Join-Path', 'Split-Path',
    'Test-Path', 'Get-ChildItem', 'Remove-Item', 'Get-Item', 'Read-YesNo',
    'Stop-Pulse', 'Remove-PulseAutostart', 'Remove-KnownShortcut',
    'Get-PulseDataFiles', 'Remove-EmptyDirectory')
foreach ($block in @($functionNodes) + @($mainAst)) {
    foreach ($command in $block.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.CommandAst]
    }, $true)) {
        if ($allowed -notcontains $command.GetCommandName()) {
            throw ("Unmocked command in extracted code: " + $command.Extent.Text)
        }
    }
}

$preamble = @'
$ErrorActionPreference = 'Stop'
$env:LOCALAPPDATA = Join-Path $env:PULSE_UNINSTALL_TEST_ROOT 'LocalAppData'
$env:APPDATA = Join-Path $env:PULSE_UNINSTALL_TEST_ROOT 'AppData'
$script:Removed = @()
$script:ReadDone = $false
function Write-TestLog([string]$Text) {
    $encoding = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::AppendAllText($env:PULSE_UNINSTALL_TEST_LOG, $Text + [System.Environment]::NewLine, $encoding)
}
function Write-Host {
    [CmdletBinding()]
    param([Parameter(Position = 0)]$Object)
    Write-TestLog ('HOST:' + [string]$Object)
}
function Read-Host {
    [CmdletBinding()]
    param([Parameter(Position = 0)]$Prompt)
    Write-TestLog 'READ'
    if ($script:ReadDone -or [string]::IsNullOrWhiteSpace($env:PULSE_UNINSTALL_TEST_ANSWER)) {
        throw 'Unexpected prompt; no answer is available.'
    }
    $script:ReadDone = $true
    return $env:PULSE_UNINSTALL_TEST_ANSWER
}
function Assert-TestPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = [System.IO.Path]::GetFullPath($env:PULSE_UNINSTALL_TEST_ROOT).TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ('I/O outside test fixture: ' + $full)
    }
    return $full
}
function Test-Removed([string]$Path) {
    foreach ($entry in $script:Removed) {
        if ([string]::Equals($Path, $entry.Path, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
        if ($entry.Recurse -and $Path.StartsWith($entry.Path.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}
function Test-Path {
    [CmdletBinding()]
    param([Parameter(Position = 0)]$Path, $LiteralPath, $PathType)
    $target = if ($LiteralPath) { [string]$LiteralPath } else { [string]$Path }
    $target = Assert-TestPath $target
    if (Test-Removed $target) { return $false }
    if ($PathType) { return Microsoft.PowerShell.Management\Test-Path -LiteralPath $target -PathType $PathType }
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $target
}
function Remove-Item {
    [CmdletBinding()]
    param($LiteralPath, [switch]$Recurse, [switch]$Force)
    $target = Assert-TestPath ([string]$LiteralPath)
    $script:Removed += [pscustomobject]@{ Path = $target; Recurse = [bool]$Recurse }
    Write-TestLog ('REMOVE:' + $target + '|' + [int][bool]$Recurse)
}
function Get-Item {
    [CmdletBinding()]
    param($LiteralPath, [switch]$Force)
    $target = Assert-TestPath ([string]$LiteralPath)
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw 'Get-Item target is missing.' }
    # The main flow can clear IsReadOnly on this copy, never on a real file.
    return [pscustomobject]@{ FullName = $target; IsReadOnly = $true }
}
function Get-ChildItem {
    [CmdletBinding()]
    param($LiteralPath, [switch]$Force)
    $target = Assert-TestPath ([string]$LiteralPath)
    Microsoft.PowerShell.Management\Get-ChildItem -LiteralPath $target -Force | Where-Object {
        -not (Test-Removed $_.FullName)
    }
}
function Stop-Pulse { Write-TestLog 'STOP' }
function Remove-PulseAutostart { Write-TestLog 'AUTOSTART' }
function Remove-KnownShortcut([string]$ShortcutPath, [string]$Label) {
    Write-TestLog ('SHORTCUT:' + $ShortcutPath)
}
function Get-Process { throw 'Real process inspection must not run.' }
function Stop-Process { throw 'Real process termination must not run.' }
function Start-Process { throw 'Real process launch must not run.' }
function Remove-ItemProperty { throw 'Registry mutation must not run.' }
function Set-ItemProperty { throw 'Registry mutation must not run.' }
'@

$observe = @'
$observed = @(Get-PulseDataFiles)
Write-TestLog ('COUNT:' + $observed.Count)
foreach ($value in $observed) {
    if ($null -eq $value) { Write-TestLog 'TYPE:null' }
    else { Write-TestLog ('TYPE:' + $value.GetType().FullName) }
    Write-TestLog ('DATA:' + [string]$value)
}
'@

$tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
$testRoot = Join-Path $tempBase ('pulse_uninstall_params_' + [guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($testRoot)
$failures = New-Object 'System.Collections.Generic.List[string]'
$utf8Bom = New-Object System.Text.UTF8Encoding $true
# The uninstaller targets Windows PowerShell 5.1 even when this harness runs in pwsh.
$powershellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

function Write-ChildScript([string]$Path, [bool]$LegacyReturn) {
    $text = $ast.ParamBlock.Extent.Text + [System.Environment]::NewLine + $preamble
    foreach ($node in $functionNodes) {
        $body = $node.Extent.Text
        if ($LegacyReturn -and $node.Name -eq 'Get-PulseDataFiles') {
            # Reintroduce only the old array wrapper as a negative control.
            $body = $body.Replace('return $result', 'return ,$result')
        }
        $text += [System.Environment]::NewLine + $body
    }
    $text += [System.Environment]::NewLine + $observe
    $text += [System.Environment]::NewLine + $mainAst.Extent.Text
    $text += [System.Environment]::NewLine + 'exit 0' + [System.Environment]::NewLine
    [System.IO.File]::WriteAllText($Path, $text, $utf8Bom)
}

function Invoke-UninstallCase {
    param([string]$Name, [string[]]$Files, [string[]]$Arguments,
        [string]$Answer, [string[]]$ExpectedDeletes, [int]$WantReads = 0,
        [bool]$LegacyReturn = $false)
    $caseRoot = Join-Path $testRoot ([guid]::NewGuid().ToString('N'))
    $local = Join-Path $caseRoot 'LocalAppData\Pulse'
    $roaming = Join-Path $caseRoot 'AppData\Pulse'
    [void][System.IO.Directory]::CreateDirectory($local)
    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $roaming))
    $exe = Join-Path $local 'Pulse.exe'
    [System.IO.File]::WriteAllText($exe, 'fixture')
    $expectedData = @()
    foreach ($relative in $Files) {
        $path = Join-Path $caseRoot $relative
        [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $path))
        [System.IO.File]::WriteAllText($path, 'fixture')
        $expectedData += [System.IO.Path]::GetFullPath($path)
    }
    $childPath = Join-Path $caseRoot 'child.ps1'
    $log = Join-Path $caseRoot 'events.log'
    Write-ChildScript $childPath $LegacyReturn
    $oldRoot = $env:PULSE_UNINSTALL_TEST_ROOT
    $oldLog = $env:PULSE_UNINSTALL_TEST_LOG
    $oldAnswer = $env:PULSE_UNINSTALL_TEST_ANSWER
    try {
        $env:PULSE_UNINSTALL_TEST_ROOT = $caseRoot
        $env:PULSE_UNINSTALL_TEST_LOG = $log
        $env:PULSE_UNINSTALL_TEST_ANSWER = $Answer
        $output = @(& $powershellExe -NoProfile -ExecutionPolicy Bypass -File $childPath @Arguments 2>&1)
        $code = $LASTEXITCODE
        $text = if (Test-Path -LiteralPath $log) { [System.IO.File]::ReadAllText($log, [System.Text.Encoding]::UTF8) } else { '' }
        $records = @($text -split '[\r\n]+' | Where-Object { $_ -ne '' })
        $counts = @($records | Where-Object { $_.StartsWith('COUNT:') })
        $types = @($records | Where-Object { $_.StartsWith('TYPE:') })
        $data = @($records | Where-Object { $_.StartsWith('DATA:') } | ForEach-Object { $_.Substring(5) })
        $reads = @($records | Where-Object { $_ -eq 'READ' }).Count
        $deletes = @($records | Where-Object { $_.StartsWith('REMOVE:') } | ForEach-Object { $_.Substring(7) })
        $wantDeletes = @($ExpectedDeletes | ForEach-Object {
            $parts = $_ -split '\|', 2
            (Join-Path $caseRoot $parts[0]) + '|' + $parts[1]
        })
        $unchanged = Test-Path -LiteralPath $exe -PathType Leaf
        foreach ($path in $expectedData) { $unchanged = $unchanged -and (Test-Path -LiteralPath $path -PathType Leaf) }
        if ($LegacyReturn) {
            $ok = $code -eq 1 -and $counts.Count -eq 1 -and $counts[0] -eq 'COUNT:1' -and
                $types.Count -eq 1 -and $types[0] -eq 'TYPE:System.Object[]' -and
                $text.Contains('StartsWith') -and $unchanged -and $deletes.Count -eq 0
        } else {
            $countOk = $counts.Count -eq 1 -and $counts[0] -eq ('COUNT:' + $Files.Count)
            $typesOk = $types.Count -eq $Files.Count -and @($types | Where-Object { $_ -ne 'TYPE:System.String' }).Count -eq 0
            $dataOk = (($data | Sort-Object) -join '|') -ceq (($expectedData | Sort-Object) -join '|')
            $deletesOk = (($deletes | Sort-Object) -join ';') -ceq (($wantDeletes | Sort-Object) -join ';')
            $programs = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Programs)
            $desktop = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Desktop)
            $wantShortcuts = @()
            foreach ($folder in @($programs, $desktop)) {
                if (-not [string]::IsNullOrWhiteSpace($folder)) { $wantShortcuts += 'SHORTCUT:' + (Join-Path $folder 'Pulse.lnk') }
            }
            $shortcuts = @($records | Where-Object { $_.StartsWith('SHORTCUT:') })
            $shortcutsOk = (($shortcuts | Sort-Object) -join ';') -ceq (($wantShortcuts | Sort-Object) -join ';')
            $ok = $code -eq 0 -and $countOk -and $typesOk -and $dataOk -and $deletesOk -and
                $reads -eq $WantReads -and (-not $text.Contains('StartsWith')) -and $unchanged -and
                $records -contains 'STOP' -and $records -contains 'AUTOSTART' -and $shortcutsOk
        }
        if ($ok) { Write-Host ('ok ' + $Name) }
        else {
            $failures.Add($Name)
            Write-Host ("FAIL {0} exit={1} reads={2}" -f $Name, $code, $reads)
            Write-Host $text
            Write-Host ($output -join [System.Environment]::NewLine)
        }
    } finally {
        $env:PULSE_UNINSTALL_TEST_ROOT = $oldRoot
        $env:PULSE_UNINSTALL_TEST_LOG = $oldLog
        $env:PULSE_UNINSTALL_TEST_ANSWER = $oldAnswer
    }
}

try {
    Invoke-UninstallCase -Name 'no data default' -Files @() -Arguments @() -ExpectedDeletes @('LocalAppData\Pulse|1')
    Invoke-UninstallCase -Name 'no data RemoveConfig' -Files @() -Arguments @('-RemoveConfig') -ExpectedDeletes @('LocalAppData\Pulse|1')
    Invoke-UninstallCase -Name 'no data KeepConfig' -Files @() -Arguments @('-KeepConfig') -ExpectedDeletes @('LocalAppData\Pulse\Pulse.exe|0', 'LocalAppData\Pulse|0')
    Invoke-UninstallCase -Name 'settings RemoveConfig' -Files @('LocalAppData\Pulse\settings.ini') -Arguments @('-RemoveConfig') -ExpectedDeletes @('LocalAppData\Pulse|1')
    Invoke-UninstallCase -Name 'settings KeepConfig' -Files @('LocalAppData\Pulse\settings.ini') -Arguments @('-KeepConfig') -ExpectedDeletes @('LocalAppData\Pulse\Pulse.exe|0')
    Invoke-UninstallCase -Name 'local and roaming logs' -Files @('LocalAppData\Pulse\pulse_net.log', 'AppData\Pulse\pulse_net.log') -Arguments @('-RemoveConfig') -ExpectedDeletes @('AppData\Pulse\pulse_net.log|0', 'LocalAppData\Pulse|1', 'AppData\Pulse|0')
    Invoke-UninstallCase -Name 'rotated log only' -Files @('LocalAppData\Pulse\pulse_net.log.1') -Arguments @('-RemoveConfig') -ExpectedDeletes @('LocalAppData\Pulse|1')
    Invoke-UninstallCase -Name 'diagnostics only' -Files @('LocalAppData\Pulse\power-diagnostics.txt') -Arguments @('-RemoveConfig') -ExpectedDeletes @('LocalAppData\Pulse|1')
    Invoke-UninstallCase -Name 'roaming rotated log only' -Files @('AppData\Pulse\pulse_net.log.1') -Arguments @('-RemoveConfig') -ExpectedDeletes @('AppData\Pulse\pulse_net.log.1|0', 'LocalAppData\Pulse|1', 'AppData\Pulse|0')
    Invoke-UninstallCase -Name 'roaming diagnostics only' -Files @('AppData\Pulse\power-diagnostics.txt') -Arguments @('-RemoveConfig') -ExpectedDeletes @('AppData\Pulse\power-diagnostics.txt|0', 'LocalAppData\Pulse|1', 'AppData\Pulse|0')
    Invoke-UninstallCase -Name 'interactive Y' -Files @('LocalAppData\Pulse\settings.ini', 'AppData\Pulse\pulse_net.log') -Arguments @() -Answer 'Y' -WantReads 1 -ExpectedDeletes @('AppData\Pulse\pulse_net.log|0', 'LocalAppData\Pulse|1', 'AppData\Pulse|0')
    Invoke-UninstallCase -Name 'interactive N' -Files @('LocalAppData\Pulse\settings.ini', 'AppData\Pulse\pulse_net.log') -Arguments @() -Answer 'N' -WantReads 1 -ExpectedDeletes @('LocalAppData\Pulse\Pulse.exe|0')
    Invoke-UninstallCase -Name 'legacy nested-array negative control' -Files @() -Arguments @('-RemoveConfig') -LegacyReturn $true -ExpectedDeletes @()
} finally {
    # This is harness cleanup only. The uninstaller never deletes real files.
    # Resolve and verify the exact generated temporary target before recursion.
    $cleanupRoot = [System.IO.Path]::GetFullPath($testRoot)
    $tempPrefix = $tempBase.TrimEnd('\') + '\'
    if (-not $cleanupRoot.StartsWith($tempPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $cleanupRoot) -notmatch '^pulse_uninstall_params_[0-9a-f]{32}$') {
        throw ('Refusing unsafe fixture cleanup: ' + $cleanupRoot)
    }
    Remove-Item -LiteralPath $cleanupRoot -Recurse -Force
}

if ($failures.Count -gt 0) {
    Write-Host ('FAILED ' + $failures.Count)
    exit 1
}
Write-Host 'uninstall_params_selftest: PASS'
exit 0
