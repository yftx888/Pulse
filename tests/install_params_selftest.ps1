# Windows PowerShell 5.1. Parses scripts\install.ps1 and runs its real main
# try, Fail, and Read-YesNo in a child process. Side effects are replaced.
# This file must not invoke the installer itself.
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$installPath = Join-Path $root 'scripts\install.ps1'
if (-not (Test-Path -LiteralPath $installPath -PathType Leaf)) {
    throw "Missing installer: $installPath"
}

$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($installPath, [ref]$tokens, [ref]$errors)
if ($errors -and $errors.Count -gt 0) {
    throw ("Installer parse errors: " + $errors[0].ToString())
}

$failures = New-Object System.Collections.Generic.List[string]
function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
    Write-Host ("FAIL " + $Message)
}

$expectedSwitches = @('Desktop', 'Autostart', 'Quiet')
if (-not $ast.ParamBlock) {
    Add-Failure 'Installer has no param block.'
} else {
    foreach ($name in $expectedSwitches) {
        $found = $false
        foreach ($parameter in $ast.ParamBlock.Parameters) {
            if ($parameter.Name.VariablePath.UserPath -ne $name) { continue }
            $found = $true
            $isSwitch = $false
            foreach ($attribute in $parameter.Attributes) {
                $typeName = ''
                if ($attribute.TypeName) { $typeName = $attribute.TypeName.Name }
                if ($typeName -eq 'switch' -or $typeName -eq 'Switch' -or $typeName -eq 'SwitchParameter') { $isSwitch = $true }
            }
            if (-not $isSwitch) { Add-Failure ("Parameter {0} is not a switch." -f $name) }
        }
        if (-not $found) { Add-Failure ("Missing switch -{0}." -f $name) }
    }
}

$assignments = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst]
}, $true)
foreach ($assignment in $assignments) {
    if ($assignment.Left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
    $left = $assignment.Left.VariablePath.UserPath
    foreach ($name in $expectedSwitches) {
        if ($left.Equals($name, [System.StringComparison]::OrdinalIgnoreCase)) {
            Add-Failure ("Assignment collides with switch {0}: {1}" -f $name, $assignment.Extent.Text)
        }
    }
}

$hasDesktopFolder = $false
$variables = $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.VariableExpressionAst]
}, $true)
foreach ($variable in $variables) {
    if ($variable.VariablePath.UserPath -eq 'desktopFolder') { $hasDesktopFolder = $true }
}
if (-not $hasDesktopFolder) { Add-Failure 'Installer no longer uses $desktopFolder.' }

function Get-FunctionAst([string]$Name) {
    foreach ($node in $ast.FindAll({
        param($item)
        $item -is [System.Management.Automation.Language.FunctionDefinitionAst]
    }, $true)) {
        if ($node.Name -eq $Name) { return $node }
    }
    return $null
}

$failAst = Get-FunctionAst 'Fail'
$readAst = Get-FunctionAst 'Read-YesNo'
if (-not $failAst) { Add-Failure 'Missing function Fail.' }
if (-not $readAst) { Add-Failure 'Missing function Read-YesNo.' }

$tryAst = $null
foreach ($node in $ast.FindAll({
    param($item)
    $item -is [System.Management.Automation.Language.TryStatementAst]
}, $true)) {
    if ($node.Extent.Text -match '\$desktopFolder' -and $node.Extent.Text -match 'Start-Process') {
        $tryAst = $node
    }
}
if (-not $tryAst) { Add-Failure 'Could not find the installer main try that uses $desktopFolder.' }

$yesWord = $null
$noWord = $null
if ($readAst) {
    $patterns = @($readAst.FindAll({
        param($item)
        $item -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $item.Value -like '^*'
    }, $true))
    if ($patterns.Count -lt 2) {
        Add-Failure 'Read-YesNo does not contain two match patterns.'
    } else {
        $yesParts = @(($patterns[0].Value -replace '[\^\(\)\$]', '') -split '\|')
        $noParts = @(($patterns[1].Value -replace '[\^\(\)\$]', '') -split '\|')
        $yesWord = $yesParts[$yesParts.Count - 1]
        $noWord = $noParts[$noParts.Count - 1]
        $yesNonAscii = $false
        foreach ($char in $yesWord.ToCharArray()) { if ([int]$char -gt 127) { $yesNonAscii = $true } }
        $noNonAscii = $false
        foreach ($char in $noWord.ToCharArray()) { if ([int]$char -gt 127) { $noNonAscii = $true } }
        if (-not $yesNonAscii -or -not $noNonAscii) {
            Add-Failure 'Read-YesNo no longer accepts the non-ASCII yes/no answers.'
        }
    }
}

function Get-IfString([System.Management.Automation.Language.IfStatementAst]$IfNode, [bool]$ElseBranch) {
    $clause = $IfNode.Clauses[0]
    $block = if ($ElseBranch) { $IfNode.ElseClause } else { $clause.Item2 }
    if (-not $block) { return $null }
    $commands = @($block.FindAll({
        param($item)
        $item -is [System.Management.Automation.Language.CommandAst] -and $item.GetCommandName() -eq 'Write-Host'
    }, $true))
    if ($commands.Count -lt 1) { return $null }
    # Search the first argument, not CommandAst itself: its first string is the
    # command name "Write-Host". Parenthesized -f text can be an expandable AST.
    if ($commands[0].CommandElements.Count -lt 2) { return $null }
    $strings = @($commands[0].CommandElements[1].FindAll({
        param($item)
        $item -is [System.Management.Automation.Language.StringConstantExpressionAst] -or
        $item -is [System.Management.Automation.Language.ExpandableStringExpressionAst]
    }, $true))
    if ($strings.Count -lt 1) { return $null }
    $value = $strings[0].Value
    $marker = $value.IndexOf('{0}')
    if ($marker -ge 0) { return $value.Substring(0, $marker) }
    return $value
}

$desktopCreated = $null
$desktopSkipped = $null
$autostartCreated = $null
$autostartSkipped = $null
if ($tryAst) {
    foreach ($node in $tryAst.FindAll({
        param($item)
        $item -is [System.Management.Automation.Language.IfStatementAst]
    }, $true)) {
        $condition = $node.Clauses[0].Item1.Extent.Text.Trim()
        if ($condition -eq '$createDesktop') {
            $desktopCreated = Get-IfString $node $false
            $desktopSkipped = Get-IfString $node $true
        } elseif ($condition -eq '$enableAutostart') {
            $autostartCreated = Get-IfString $node $false
            $autostartSkipped = Get-IfString $node $true
        }
    }
}
if ([string]::IsNullOrWhiteSpace($desktopCreated) -or [string]::IsNullOrWhiteSpace($desktopSkipped) -or
    [string]::IsNullOrWhiteSpace($autostartCreated) -or [string]::IsNullOrWhiteSpace($autostartSkipped)) {
    Add-Failure 'Could not extract the desktop/autostart result messages from the installer AST.'
}

if ($failures.Count -gt 0) {
    Write-Host ("AST failures: {0}" -f $failures.Count)
    exit 1
}

$programs = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Programs)
$desktop = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Desktop)
$startLink = Join-Path $programs 'Pulse.lnk'
$desktopLink = Join-Path $desktop 'Pulse.lnk'
$isolatedRoot = 'C:\PulseTestIsolated'
$dest = 'C:\PulseTestIsolated\LocalAppData\Pulse\Pulse.exe'
$source = 'C:\PulseFakeSource\Pulse.exe'

$childPath = Join-Path ([System.IO.Path]::GetTempPath()) ('pulse_install_params_' + [guid]::NewGuid().ToString('N') + '.ps1')
$child = New-Object System.Text.StringBuilder
[void]$child.AppendLine($ast.ParamBlock.Extent.Text)
[void]$child.AppendLine('$ErrorActionPreference = ''Stop''')
[void]$child.AppendLine('$env:LOCALAPPDATA = ''C:\PulseTestIsolated\LocalAppData''')
[void]$child.AppendLine('function Write-Log([string]$Message) {')
[void]$child.AppendLine('    $utf8 = New-Object System.Text.UTF8Encoding $false')
[void]$child.AppendLine('    [System.IO.File]::AppendAllText($env:PULSE_TEST_LOG, ($Message + [char]13 + [char]10), $utf8)')
[void]$child.AppendLine('}')
[void]$child.AppendLine('function Write-Host {')
[void]$child.AppendLine('    [CmdletBinding()]')
[void]$child.AppendLine('    param([Parameter(Position = 0)]$Object)')
[void]$child.AppendLine('    Write-Log ([string]$Object)')
[void]$child.AppendLine('}')
[void]$child.AppendLine('function Read-Host {')
[void]$child.AppendLine('    [CmdletBinding()]')
[void]$child.AppendLine('    param([Parameter(Position = 0)]$Prompt)')
[void]$child.AppendLine('    $answers = @($env:PULSE_TEST_ANSWERS -split '','')')
[void]$child.AppendLine('    $index = 0')
[void]$child.AppendLine('    if ($env:PULSE_TEST_ANSWER_INDEX) { $index = [int]$env:PULSE_TEST_ANSWER_INDEX }')
[void]$child.AppendLine('    if ($index -ge $answers.Count -or [string]::IsNullOrWhiteSpace($answers[$index])) { throw ''unexpected Read-Host'' }')
[void]$child.AppendLine('    $answer = $answers[$index]')
[void]$child.AppendLine('    $env:PULSE_TEST_ANSWER_INDEX = [string]($index + 1)')
[void]$child.AppendLine('    Write-Log (''READ:'' + $answer)')
[void]$child.AppendLine('    return $answer')
[void]$child.AppendLine('}')
[void]$child.AppendLine('function Test-Path {')
[void]$child.AppendLine('    [CmdletBinding()]')
[void]$child.AppendLine('    param($LiteralPath, $Path, $PathType)')
[void]$child.AppendLine('    $target = if ($LiteralPath) { [string]$LiteralPath } else { [string]$Path }')
[void]$child.AppendLine('    if ($PathType -eq ''Leaf'' -and $target -like ''*\Pulse.exe'') { return $true }')
[void]$child.AppendLine('    return $false')
[void]$child.AppendLine('}')
[void]$child.AppendLine('function New-Item { [CmdletBinding()] param($ItemType, $Path, $LiteralPath, [switch]$Force) Write-Log ''NEWITEM'' }')
[void]$child.AppendLine('function Copy-Item { [CmdletBinding()] param($LiteralPath, $Path, $Destination, [switch]$Force) Write-Log ''COPY'' }')
[void]$child.AppendLine('function Get-Item { throw ''Get-Item must not run'' }')
[void]$child.AppendLine('function Get-Process { throw ''Get-Process must not run'' }')
[void]$child.AppendLine('function Stop-Process { throw ''Stop-Process must not run'' }')
[void]$child.AppendLine('function Find-PulseExe { return ''C:\PulseFakeSource\Pulse.exe'' }')
[void]$child.AppendLine('function Stop-Pulse { Write-Log ''STOP'' }')
[void]$child.AppendLine('function New-PulseShortcut {')
[void]$child.AppendLine('    [CmdletBinding()]')
[void]$child.AppendLine('    param([string]$ShortcutPath, [string]$Target)')
[void]$child.AppendLine('    Write-Log (''SHORTCUT:'' + $ShortcutPath)')
[void]$child.AppendLine('}')
[void]$child.AppendLine('function Set-PulseAutostart {')
[void]$child.AppendLine('    [CmdletBinding()]')
[void]$child.AppendLine('    param([string]$Target)')
[void]$child.AppendLine('    Write-Log (''AUTOSTART:'' + $Target)')
[void]$child.AppendLine('}')
[void]$child.AppendLine('function Start-Process {')
[void]$child.AppendLine('    [CmdletBinding()]')
[void]$child.AppendLine('    param($FilePath, $WorkingDirectory, $ArgumentList, $Verb, $WindowStyle)')
[void]$child.AppendLine('    Write-Log (''START:'' + [string]$FilePath)')
[void]$child.AppendLine('}')
[void]$child.AppendLine($failAst.Extent.Text)
[void]$child.AppendLine($readAst.Extent.Text)
[void]$child.AppendLine($tryAst.Extent.Text)
$utf8Bom = New-Object System.Text.UTF8Encoding $true
[System.IO.File]::WriteAllText($childPath, $child.ToString(), $utf8Bom)

function Invoke-InstallCase([string]$Name, [string]$Answers, [string[]]$Arguments, [bool]$WantDesktop, [bool]$WantAutostart, [int]$WantReads) {
    $log = Join-Path ([System.IO.Path]::GetTempPath()) ("pulse_install_params_" + [guid]::NewGuid().ToString('N') + '.log')
    $previousAnswers = $env:PULSE_TEST_ANSWERS
    $previousLog = $env:PULSE_TEST_LOG
    $previousIndex = $env:PULSE_TEST_ANSWER_INDEX
    try {
        $env:PULSE_TEST_ANSWERS = $Answers
        $env:PULSE_TEST_LOG = $log
        $env:PULSE_TEST_ANSWER_INDEX = '0'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $childPath @Arguments
        $code = $LASTEXITCODE
        $text = ''
        if (Test-Path -LiteralPath $log) {
            $text = [System.IO.File]::ReadAllText($log, [System.Text.Encoding]::UTF8)
        }
        $reads = ([regex]::Matches($text, '(?m)^READ:')).Count
        $shortcuts = [regex]::Matches($text, '(?m)^SHORTCUT:(.*)$')
        $shortcutPaths = @()
        foreach ($match in $shortcuts) { $shortcutPaths += $match.Groups[1].Value.Trim() }
        $hasStart = $shortcutPaths -contains $startLink
        $hasDesktop = $shortcutPaths -contains $desktopLink
        $hasAutostart = $text.Contains('AUTOSTART:' + $dest)
        $started = $text.Contains('START:' + $dest)
        $ok = ($code -eq 0) -and $hasStart -and $started -and $text.Contains('STOP') -and $text.Contains('COPY') -and
            $text.Contains('NEWITEM') -and $text.Contains($source) -and $text.Contains($dest) -and ($reads -eq $WantReads) -and
            ($hasDesktop -eq $WantDesktop) -and ($hasAutostart -eq $WantAutostart)
        if ($WantDesktop) { $ok = $ok -and $text.Contains($desktopCreated) -and (-not $text.Contains($desktopSkipped)) }
        else { $ok = $ok -and $text.Contains($desktopSkipped) -and (-not $text.Contains($desktopCreated)) }
        if ($WantAutostart) { $ok = $ok -and $text.Contains($autostartCreated) -and (-not $text.Contains($autostartSkipped)) }
        else { $ok = $ok -and $text.Contains($autostartSkipped) -and (-not $text.Contains($autostartCreated)) }
        if (-not $ok) {
            Add-Failure ("Case {0} exit={1} reads={2} desktop={3} autostart={4}`n{5}" -f $Name, $code, $reads, $hasDesktop, $hasAutostart, $text)
        } else {
            Write-Host ("ok " + $Name)
        }
    } finally {
        $env:PULSE_TEST_ANSWERS = $previousAnswers
        $env:PULSE_TEST_LOG = $previousLog
        $env:PULSE_TEST_ANSWER_INDEX = $previousIndex
        if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    }
}

Invoke-InstallCase 'interactive Y/Y' 'Y,Y' @() $true $true 2
Invoke-InstallCase 'interactive Y/N' 'Y,N' @() $true $false 2
Invoke-InstallCase 'interactive N/Y' 'N,Y' @() $false $true 2
Invoke-InstallCase 'interactive N/N' 'N,N' @() $false $false 2
if ($yesWord -and $noWord) {
    Invoke-InstallCase 'interactive localized yes/no' ($yesWord + ',' + $noWord) @() $true $false 2
}
Invoke-InstallCase 'switch Desktop' '' @('-Desktop') $true $false 0
Invoke-InstallCase 'switch Autostart' '' @('-Autostart') $false $true 0
Invoke-InstallCase 'switches Desktop+Autostart' '' @('-Desktop', '-Autostart') $true $true 0
Invoke-InstallCase 'switch Quiet' '' @('-Quiet') $false $false 0
Invoke-InstallCase 'Quiet with both switches' '' @('-Quiet', '-Desktop', '-Autostart') $true $true 0

if (Test-Path -LiteralPath $isolatedRoot) {
    Add-Failure 'Isolated LOCALAPPDATA path was created on disk.'
}
if (Test-Path -LiteralPath $childPath) { Remove-Item -LiteralPath $childPath -Force }

if ($failures.Count -gt 0) {
    Write-Host ("FAILED {0}" -f $failures.Count)
    exit 1
}
Write-Host 'install_params_selftest: PASS'
exit 0
