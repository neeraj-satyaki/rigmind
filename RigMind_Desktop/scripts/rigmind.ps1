<#
.SYNOPSIS
  One entry point for RigMind's automatic install, uninstall, tests and full cycle (visible, nothing hidden).

.DESCRIPTION
  .\rigmind.ps1 <command> [options]

  install      Visible installer driven automatically: licence, install, the sample-data and BitNet questions, the downloads, Finish,
               then checks of the installed files, GPU runtime, BitNet model, shortcuts and uninstall entry.
  uninstall    Visible uninstaller driven automatically, then checks that nothing is left. -ClearData also removes %LOCALAPPDATA%\RigMind.
  test         Automated tests. -Suite picks what runs (comma separated): unit, cases, ui, e2e, load, or all (= unit,cases,ui,e2e).
                 unit   the C++ test programs in build-app\Release
                 cases  the generated bridge cases (about 24,000 checks) against the C++ backend
                 ui     TypeScript type check and the UI tests (vitest)
                 e2e    the visible app test on the INSTALLED app (open, sign-in, camera, every page, live data, reports, settings)
                 load   the two-stream CPU/GPU load test (about 16 minutes)
               -Build builds the UI bundle and the C++ projects first. A report is written to tests\run-results\run-<time>.md.
  cycle        The whole round trip: uninstall old -> install -> app test -> uninstall. -KeepInstalled stops before the last uninstall.
  hooks        Installs a git pre-push hook that runs unit, cases and ui before every push (-Remove takes it out).
  help         This text.

  Installing and uninstalling need administrator rights: the script asks once (UAC) and continues in a visible elevated window.
  Do not touch the mouse while a wizard is driven. The e2e suite must run from a NORMAL (not elevated) window: it cannot attach to
  an app that was started with administrator rights.

.EXAMPLE   .\rigmind.ps1 install
.EXAMPLE   .\rigmind.ps1 install -Installer D:\drop\RigMind-1.0.0-win64-r14.exe -Demo yes -Bitnet no
.EXAMPLE   .\rigmind.ps1 test                       # unit, cases, ui
.EXAMPLE   .\rigmind.ps1 test -Suite all -Build
.EXAMPLE   .\rigmind.ps1 cycle
.EXAMPLE   .\rigmind.ps1 uninstall -ClearData -Yes
#>
param(
    [Parameter(Position = 0)][ValidateSet('help', 'install', 'uninstall', 'test', 'cycle', 'hooks', 'reinstall')][string]$Command = 'help',
    [string]$Installer,
    [ValidateSet('yes', 'no')][string]$Demo = 'no',
    [ValidateSet('yes', 'no')][string]$Bitnet = 'yes',
    [switch]$Silent,
    [switch]$ClearData,
    [switch]$Yes,
    [string]$Suite = 'unit,cases,ui',
    [switch]$Build,
    [switch]$KeepInstalled,
    [switch]$Remove,
    [switch]$Elevated
)
$ErrorActionPreference = 'Stop'

# ---- paths and helpers ------------------------------------------------------------------------------------------------------
$Desktop = Split-Path -Parent $PSScriptRoot                  # ...\RigMind_Desktop
$Repo = Split-Path -Parent $Desktop                          # repository root
$E2E = Join-Path $Desktop 'tests\e2e'
$Release = Join-Path $Desktop 'release-out'
$BuildDir = Join-Path $Desktop 'build-app'
$AppDir = Join-Path $env:ProgramFiles 'RigMind'
$Node = if (Test-Path "$env:ProgramFiles\nodejs\node.exe") { "$env:ProgramFiles\nodejs\node.exe" } else { 'node' }
$CMake = if (Test-Path "$env:ProgramFiles\CMake\bin\cmake.exe") { "$env:ProgramFiles\CMake\bin\cmake.exe" } else { 'cmake' }

function Test-Admin { ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
function Write-Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

# Runs this same script elevated with the same arguments (one UAC prompt, a visible window) and returns its exit code.
function Invoke-Elevated([string]$AsCommand) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, $AsCommand)
    foreach ($key in $PSBoundParameters.Keys) { }
    foreach ($entry in @{ Installer = $Installer; Demo = $Demo; Bitnet = $Bitnet; Suite = $Suite }.GetEnumerator()) {
        if ($entry.Value) { $arguments += "-$($entry.Key)"; $arguments += $entry.Value }
    }
    foreach ($flag in @('Silent', 'ClearData', 'Yes', 'Build', 'KeepInstalled')) { if ((Get-Variable $flag).Value) { $arguments += "-$flag" } }
    $arguments += '-Elevated'
    Write-Host 'Asking for administrator rights (approve the UAC prompt)...' -ForegroundColor Yellow
    (Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $arguments -Verb RunAs -Wait -PassThru).ExitCode
}

function Find-Installer([string]$Path) {
    if ($Path) {
        if (-not (Test-Path $Path)) { throw "Installer not found: $Path" }
        return (Resolve-Path $Path).Path
    }
    $found = Get-ChildItem -Path $Release -Filter 'RigMind-*-win64*.exe' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $found) { throw "No installer in $Release. Build one first (docs/STRUCTURE.md, section 6)." }
    $found.FullName
}

function Assert-Python { if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python is needed for the visible test driver (python on PATH with pywinauto and playwright).' } }

function Stop-InstalledRigMind {
    # only processes that run from the installed folder
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -in @('RigMind', 'rigmind-pipeline', 'llama-server') -and $_.Path -and $_.Path.StartsWith($AppDir, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Close-Window { if ($Elevated) { Read-Host 'Press Enter to close this window' | Out-Null } }

# ---- install ----------------------------------------------------------------------------------------------------------------
function Do-Install {
    $exe = Find-Installer $Installer
    Write-Step "Auto install: $(Split-Path -Leaf $exe)"
    if (-not (Test-Admin)) { return (Invoke-Elevated 'install') }
    if ($Silent) {
        Write-Host 'Silent install (no windows). Sample data: No. The NVIDIA runtime and the BitNet model are downloaded.' -ForegroundColor Yellow
        $p = Start-Process -FilePath $exe -ArgumentList '/S' -Wait -PassThru
        Write-Host "Installer exit code: $($p.ExitCode)"
        return $p.ExitCode
    }
    Assert-Python
    Push-Location $E2E
    try {
        $env:PYTHONUNBUFFERED = '1'
        python -u visible_e2e.py $exe --demo $Demo --bitnet $Bitnet --phases 1 --install-timeout 3000 2>&1 | Tee-Object -FilePath (Join-Path $E2E 'auto-install.log')
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    if ($code -eq 0) { Write-Host "`nInstalled. RESULT: PASS" -ForegroundColor Green } else { Write-Host "`nInstall checks reported failures (exit $code). See tests\e2e\auto-install.log" -ForegroundColor Red }
    Close-Window
    $code
}

# ---- uninstall --------------------------------------------------------------------------------------------------------------
function Do-Uninstall {
    Write-Step 'Auto uninstall'
    $code = 0
    if (-not (Test-Path (Join-Path $AppDir 'Uninstall.exe'))) {
        Write-Host "RigMind is not installed at $AppDir." -ForegroundColor Yellow
    } else {
        if (-not (Test-Admin)) { return (Invoke-Elevated 'uninstall') }
        Stop-InstalledRigMind
        if (Get-Command python -ErrorAction SilentlyContinue) {
            Push-Location $E2E
            try {
                $env:PYTHONUNBUFFERED = '1'
                python -u visible_e2e.py --no-install --phases 9 --app-dir $AppDir 2>&1 | Tee-Object -FilePath (Join-Path $E2E 'auto-uninstall.log')
            } finally { Pop-Location }
        } else {
            Write-Host 'Python is not available: opening the uninstaller for you to click through.' -ForegroundColor Yellow
            Start-Process -FilePath (Join-Path $AppDir 'Uninstall.exe') -Wait
        }
        $left = @()
        if (Test-Path $AppDir) { $left += $AppDir }
        if (Test-Path "$env:PUBLIC\Desktop\RigMind.lnk") { $left += "$env:PUBLIC\Desktop\RigMind.lnk" }
        if (Test-Path "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\RigMind.lnk") { $left += 'the Start menu shortcut' }
        if ($left.Count) { Write-Host "Left behind: $($left -join ', ')" -ForegroundColor Red; $code = 1 }
        else { Write-Host 'Uninstalled: nothing left in the program folder, shortcuts or uninstall list.' -ForegroundColor Green }
    }
    if ($ClearData) {
        $data = Join-Path $env:LOCALAPPDATA 'RigMind'
        if (Test-Path $data) {
            $go = [bool]$Yes
            if (-not $go) { $go = (Read-Host "Delete $data (database, footage, audit log, accounts)? Type YES") -eq 'YES' }
            if ($go) { Remove-Item -LiteralPath $data -Recurse -Force; Write-Host "Cleared $data" -ForegroundColor Green } else { Write-Host 'User data kept.' }
        } else { Write-Host 'No user data folder to clear.' }
    }
    Close-Window
    $code
}

# ---- tests ------------------------------------------------------------------------------------------------------------------
$script:Results = New-Object System.Collections.Generic.List[object]
function Add-Result([string]$Suite, [string]$Name, [bool]$Ok, [string]$Detail) {
    $script:Results.Add([pscustomobject]@{ Suite = $Suite; Name = $Name; Status = $(if ($Ok) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
    $color = if ($Ok) { 'Green' } else { 'Red' }
    Write-Host ("  {0,-5} {1,-28} {2}" -f $(if ($Ok) { 'PASS' } else { 'FAIL' }), $Name, $Detail) -ForegroundColor $color
}

function Test-Unit {
    Write-Step 'Unit tests (C++)'
    $bin = Join-Path $BuildDir 'Release'
    $names = 'backend_test', 'roi_facts_test', 'platform_overlay_test', 'tracking_test', 'scene_test', 'zone_filter_test', 'evidence_query_test',
        'chat_sessions_test', 'mask_stabilizer_test', 'multi_camera_test', 'mcp_test', 'auth_test', 'ews_plugin_test'
    foreach ($n in $names) {
        $exe = Join-Path $bin "$n.exe"
        if (-not (Test-Path $exe)) { Add-Result 'unit' $n $false 'not built (run with -Build)'; continue }
        $extra = @(if ($n -eq 'ews_plugin_test') { Join-Path $bin 'librigmindews.dll' })
        $out = & $exe @extra 2>&1 | Select-Object -Last 1
        Add-Result 'unit' $n ($LASTEXITCODE -eq 0) "$out"
    }
}

function Test-Cases {
    Write-Step 'Generated cases (about 24,000 checks)'
    Assert-Python
    $cli = Join-Path $BuildDir 'Release\rigmind-bridge-cli.exe'
    if (-not (Test-Path $cli)) { Add-Result 'cases' 'bridge cli' $false 'not built (run with -Build)'; return }
    $dir = Join-Path $Desktop 'tests\cases'
    Push-Location $dir
    try {
        python generate_cases.py | Select-Object -First 1 | Write-Host
        $csv = Join-Path $env:TEMP 'rigmind-cases-results.csv'
        $out = python run_cases.py $cli $csv 2>&1
        $out | Select-Object -Last 12 | ForEach-Object { Write-Host "  $_" }
        $last = $out | Select-Object -Last 1
        $ok = ($LASTEXITCODE -eq 0) -and ($last -match 'failed=0')
        Add-Result 'cases' 'generated cases' $ok "$last"
    } finally { Pop-Location }
}

function Test-Ui {
    Write-Step 'UI (type check and tests)'
    $ui = Join-Path $Repo 'Rigmind_v2'
    Push-Location $ui
    try {
        $tsc = & $Node node_modules\typescript\bin\tsc --noEmit 2>&1
        Add-Result 'ui' 'tsc --noEmit' ($LASTEXITCODE -eq 0) $(if ($LASTEXITCODE -eq 0) { 'clean' } else { "$($tsc | Select-Object -First 1)" })
        $vt = & $Node node_modules\vitest\vitest.mjs run 2>&1
        $line = ($vt | Select-String 'Tests ' | Select-Object -Last 1)
        Add-Result 'ui' 'vitest' ($LASTEXITCODE -eq 0) "$line".Trim()
    } finally { Pop-Location }
}

function Test-E2E {
    Write-Step 'Visible app test on the installed app'
    if (Test-Admin) { Add-Result 'e2e' 'visible app test' $false 'run this suite from a normal (not elevated) window: an elevated test cannot attach to the app'; return }
    if (-not (Test-Path (Join-Path $AppDir 'RigMind.exe'))) { Add-Result 'e2e' 'visible app test' $false "RigMind is not installed at $AppDir (run: rigmind.ps1 install)"; return }
    Assert-Python
    Push-Location $E2E
    try {
        $env:PYTHONUNBUFFERED = '1'
        $log = Join-Path $E2E 'auto-e2e.log'
        python -u visible_e2e.py --no-install --no-uninstall --app-dir $AppDir --demo $Demo --bitnet $Bitnet --phases 2,3,4,5,6,7,8 --live-seconds 180 2>&1 | Tee-Object -FilePath $log
        $code = $LASTEXITCODE
        $result = (Select-String -Path $log -Pattern '^RESULT:' | Select-Object -Last 1).Line
        $cta = (Select-String -Path $log -Pattern 'CTA coverage' | Select-Object -Last 1).Line
        Add-Result 'e2e' 'visible app test' ($code -eq 0) "$result $cta".Trim()
    } finally { Pop-Location }
}

function Test-Load {
    Write-Step 'Two-stream CPU/GPU load test (about 16 minutes)'
    Assert-Python
    Push-Location (Join-Path $Desktop 'tests\validation')
    try {
        python -u dual_stream_load.py 2>&1 | Tee-Object -FilePath (Join-Path $env:TEMP 'rigmind-dual-load.log') | Select-Object -Last 25 | ForEach-Object { Write-Host "  $_" }
        Add-Result 'load' 'dual stream load' ($LASTEXITCODE -eq 0) 'see tests\validation\DUAL_STREAM_REPORT.md'
    } finally { Pop-Location }
}

function Do-Build {
    Write-Step 'Build (UI bundle, staging, C++)'
    Push-Location (Join-Path $Repo 'Rigmind_v2')
    try { & $Node node_modules\vite\bin\vite.js build --config vite.desktop.config.ts | Select-Object -Last 1 | Write-Host } finally { Pop-Location }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'stage_web.ps1') | Select-Object -Last 1 | Write-Host
    & $CMake --build $BuildDir --config Release | Select-Object -Last 3 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw 'The build failed.' }
}

function Do-Test {
    $wanted = @($Suite.ToLower().Split(',', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { $_.Trim() })
    if ($wanted -contains 'all') { $wanted = @('unit', 'cases', 'ui', 'e2e') }
    foreach ($s in $wanted) { if ($s -notin 'unit', 'cases', 'ui', 'e2e', 'load') { throw "Unknown suite '$s' (unit, cases, ui, e2e, load, all)." } }
    if ($Build) { Do-Build }
    $started = Get-Date
    foreach ($s in 'unit', 'cases', 'ui', 'e2e', 'load') {
        if ($wanted -notcontains $s) { continue }
        try {
            switch ($s) { 'unit' { Test-Unit } 'cases' { Test-Cases } 'ui' { Test-Ui } 'e2e' { Test-E2E } 'load' { Test-Load } }
        } catch { Add-Result $s $s $false "$($_.Exception.Message)" }
    }
    $failed = @($script:Results | Where-Object Status -eq 'FAIL')
    $dir = Join-Path $Desktop 'tests\run-results'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $report = Join-Path $dir ("run-{0:yyyyMMdd-HHmmss}.md" -f $started)
    $md = @("# RigMind test run $($started.ToString('yyyy-MM-dd HH:mm'))", '', "Suites: $($wanted -join ', ') - $([int]((Get-Date) - $started).TotalSeconds) s", '', '| Suite | Test | Result | Detail |', '|---|---|---|---|')
    foreach ($r in $script:Results) { $md += "| $($r.Suite) | $($r.Name) | $($r.Status) | $(($r.Detail -replace '\|', '/')) |" }
    $md += '', "**$(@($script:Results | Where-Object Status -eq 'PASS').Count) passed, $($failed.Count) failed.**"
    Set-Content -Path $report -Value $md -Encoding UTF8
    Write-Step "Summary: $(@($script:Results | Where-Object Status -eq 'PASS').Count) passed, $($failed.Count) failed"
    Write-Host "Report: $report"
    if ($failed.Count) { 1 } else { 0 }
}

# ---- cycle ------------------------------------------------------------------------------------------------------------------
function Do-Reinstall {
    # elevated half of the cycle: remove an installed copy, then install
    $c = 0
    if (Test-Path (Join-Path $AppDir 'Uninstall.exe')) { $c = [int](Do-Uninstall | Select-Object -Last 1) }
    $c2 = [int](Do-Install | Select-Object -Last 1)
    [Math]::Max($c, $c2)
}

function Do-Cycle {
    Write-Step 'Full cycle: uninstall old -> install -> app test -> uninstall'
    if (Test-Admin) { throw 'Start the cycle from a normal (not elevated) window: the app test cannot attach to an elevated app. The script asks for administrator rights itself where needed.' }
    $a = Invoke-Elevated 'reinstall'
    if ($a -ne 0) { Write-Host "Install step failed (exit $a); not testing." -ForegroundColor Red; return $a }
    $script:Suite = 'e2e'
    $b = [int](Do-Test | Select-Object -Last 1)
    $c = 0
    if (-not $KeepInstalled) { $c = Invoke-Elevated 'uninstall' } else { Write-Host 'Leaving RigMind installed (-KeepInstalled).' }
    Write-Step "Cycle finished: install $(if ($a -eq 0) { 'PASS' } else { 'FAIL' }), app test $(if ($b -eq 0) { 'PASS' } else { 'FAIL' }), uninstall $(if ($c -eq 0) { 'PASS' } else { 'FAIL' })"
    [Math]::Max($a, [Math]::Max($b, $c))
}

# ---- git hook ---------------------------------------------------------------------------------------------------------------
function Do-Hooks {
    $hook = Join-Path $Repo '.git\hooks\pre-push'
    if ($Remove) {
        if (Test-Path $hook) { Remove-Item $hook -Force; Write-Host 'pre-push hook removed.' } else { Write-Host 'No pre-push hook installed.' }
        return 0
    }
    if (-not (Test-Path (Join-Path $Repo '.git'))) { throw "$Repo is not a git repository." }
    $script = (Join-Path $PSScriptRoot 'rigmind.ps1') -replace '\\', '/'
    $body = "#!/bin/sh`n# RigMind: run the fast tests (unit, cases, ui) before every push; a failure stops the push.`npwsh -NoProfile -ExecutionPolicy Bypass -File `"$script`" test -Suite unit,cases,ui || { echo 'Push stopped: tests failed.'; exit 1; }`n"
    [IO.File]::WriteAllText($hook, $body.Replace("`r`n", "`n"))
    Write-Host "pre-push hook installed at $hook (remove with: rigmind.ps1 hooks -Remove)." -ForegroundColor Green
    0
}

function Show-Help { Get-Help $PSCommandPath -Detailed | Out-String | Write-Host; 0 }

# ---- run --------------------------------------------------------------------------------------------------------------------
$code = switch ($Command) {
    'install'   { Do-Install }
    'uninstall' { Do-Uninstall }
    'test'      { Do-Test }
    'cycle'     { Do-Cycle }
    'reinstall' { Do-Reinstall }
    'hooks'     { Do-Hooks }
    default     { Show-Help }
}
exit ([int]($code | Select-Object -Last 1))
