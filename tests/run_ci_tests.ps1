# Run hardware-independent checks before uploading or packaging artifacts.
# Use Windows PowerShell 5.1 with the MinGW-w64 bin directory on PATH.
#Requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$testExe = Join-Path $PSScriptRoot 'feedback_selftest.exe'
# The script tests target Windows PowerShell 5.1 even when this runner is pwsh.
$powershellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

Push-Location -LiteralPath $repoRoot
try {
    & g++ -std=c++17 -O2 -static -finput-charset=UTF-8 -fexec-charset=UTF-8 -Isrc -o $testExe tests\feedback_selftest.cpp src\ui_model.cpp src\i18n.cpp -luser32
    if ($LASTEXITCODE -ne 0) { throw "feedback_selftest compilation failed (exit $LASTEXITCODE)." }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "feedback_selftest failed (exit $LASTEXITCODE)." }

    foreach ($test in @('install_params_selftest.ps1', 'uninstall_params_selftest.ps1')) {
        & $powershellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $test)
        if ($LASTEXITCODE -ne 0) { throw "$test failed (exit $LASTEXITCODE)." }
    }
    Write-Host 'run_ci_tests: PASS'
} catch {
    Write-Error -Message $_ -ErrorAction Continue
    exit 1
} finally {
    Pop-Location
}
exit 0
