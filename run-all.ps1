# RustStrike one-click launcher.
#
# Starts the Go service core (implant TCP :4444 + operator HTTP :8091), the
# Rust implant (reverse-connects to the core), and the Wails desktop GUI.
# Each runs in its own window; Ctrl+C in any window stops that piece. Closing
# this window stops everything.
#
# Usage:
#   .\run-all.ps1                # core + implant + GUI
#   .\run-all.ps1 -NoGui         # core + implant only (headless, drive via REST)
#   .\run-all.ps1 -NoImplant     # core + GUI only (connect an implant manually)
#
# Override ports / core URL / operator auth via the usual env vars
# (RUSTSTRIKE_CONFIG, RUSTSTRIKE_BOFS, RUSTSTRIKE_CORE,
# RUSTSTRIKE_AUTH_USERNAME, RUSTSTRIKE_AUTH_PASSWORD, RUSTSTRIKE_AUTH_TOKEN)
# -- they're forwarded to the child processes. If no config/env auth is set,
# this launcher generates one-time credentials for the current run.
#
# Prereqs (built if missing):
#   - target\release\ruststrike-implant.exe   (cargo build --release)
#   - clients\server\server.exe         (go build)
#   - clients\client\build\bin\xstrike.exe (wails build)
#   - clients\server\bofs\*.x64.o          (gcc -c examples/*.c + copy)

[CmdletBinding()]
param(
  [switch]$NoGui,
  [switch]$NoImplant,
  [string]$TcpPort = '4444',
  [string]$HttpPort = '8091'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

# --- locate toolchain binaries -------------------------------------------------
$implantExe = Join-Path $root 'target\release\ruststrike-implant.exe'
$coreExe    = Join-Path $root 'clients\server\server.exe'
$guiExe     = Join-Path $root 'clients\client\build\bin\xstrike.exe'
$bofsDir    = Join-Path $root 'clients\server\bofs'
$configPath = if ($env:RUSTSTRIKE_CONFIG) { $env:RUSTSTRIKE_CONFIG } else { Join-Path $root 'ruststrike.config.json' }
$configDir = Split-Path -Parent $configPath
if (-not $configDir) { $configDir = $root }
$config = $null
$configAuthToken = ''
$configAuthUsername = ''
$configAuthPassword = ''

function Resolve-ConfigPath($p) {
  if (-not $p) { return '' }
  if ([IO.Path]::IsPathRooted([string]$p)) { return [string]$p }
  return Join-Path $configDir ([string]$p)
}

if (Test-Path $configPath) {
  $env:RUSTSTRIKE_CONFIG = $configPath
  $config = Get-Content -Raw $configPath | ConvertFrom-Json
  Write-Host "[config] using $configPath" -ForegroundColor DarkGray
  if (-not $PSBoundParameters.ContainsKey('TcpPort') -and $config.server.implant_tcp_port) {
    $TcpPort = [string]$config.server.implant_tcp_port
  }
  if (-not $PSBoundParameters.ContainsKey('HttpPort') -and $config.server.operator_http_port) {
    $HttpPort = [string]$config.server.operator_http_port
  }
  if (-not $env:RUSTSTRIKE_BOFS -and $config.paths.bof_dir) {
    $bofsDir = Resolve-ConfigPath $config.paths.bof_dir
  }
  if ($config.auth.token) { $configAuthToken = [string]$config.auth.token }
  if ($config.auth.username) { $configAuthUsername = [string]$config.auth.username }
  if ($config.auth.password) { $configAuthPassword = [string]$config.auth.password }
}

function Ensure-Built($label, $exe, $buildCmd, $buildCwd) {
  if (Test-Path $exe) { Write-Host "[ok] $label found" -ForegroundColor DarkGray; return }
  Write-Host "[build] $label missing ... building ..." -ForegroundColor Yellow
  & $buildCmd
  if ($LASTEXITCODE -ne 0) { throw "$label build failed (exit $LASTEXITCODE)" }
  if (-not (Test-Path $exe)) { throw "$label build finished but $exe not found" }
  Write-Host "[ok] $label built" -ForegroundColor Green
}

function Ensure-Bofs() {
  if (-not (Test-Path $bofsDir)) { New-Item -ItemType Directory -Force $bofsDir | Out-Null }
  $expected = @('hello','cmd_exec','powershell_exec','winapi_exec','ps','proc_list','proc_kill','netstat','ls','file_list','download','file_download','screenshot','upload','shellcode_exec','shellcode_exec_nt','sysinfo','bof_whoami','proc_critical_set','proc_critical_unset','schtask_persist','schtask_persist_xml','schtask_persist_reg','svc_create_api','user_create_net','user_create_cmd','user_create_ps')
  $have = @(Get-ChildItem -Path $bofsDir -Filter '*.x64.o' -ErrorAction SilentlyContinue | ForEach-Object { $_.BaseName })
  $missing = $expected | Where-Object { $have -notcontains $_ }
  if ($missing.Count -eq 0) { Write-Host "[ok] BOF library has $($have.Count) BOF(s)" -ForegroundColor DarkGray; return }
  Write-Host "[build] staging BOFs into $bofsDir (missing: $($missing -join ', '))..." -ForegroundColor Yellow
  $examples = Join-Path $root 'examples'
  foreach ($c in @('hello.c','cmd_exec.c','powershell_exec.c','winapi_exec.c','ps.c','proc_list.c','proc_kill.c','netstat.c','ls.c','file_list.c','download.c','file_download.c','screenshot.c','upload.c','shellcode_exec.c','shellcode_exec_nt.c','sysinfo.c')) {
    $src = Join-Path $examples $c
    $obj = Join-Path $examples ([IO.Path]::GetFileNameWithoutExtension($c) + '.x64.o')
    if (-not (Test-Path $obj)) {
      & gcc -c $src -o $obj
      if ($LASTEXITCODE -ne 0) { Write-Warning "  gcc failed on $c ... skipping (BOF optional)" }
    }
    if (Test-Path $obj) { Copy-Item $obj $bofsDir -Force }
  }
  # Project-root bofs/ tree: the richer CS-packed BOF library (persistence,
  # user_mgmt, process, recon). Built with -I bofs so #include "beacon.h"
  # resolves. Only the BOFs not already provided by examples/ are staged.
  $bofsTree = Join-Path $root 'bofs'
  foreach ($c in @('info\bof_whoami.c','process\proc_critical_set.c','process\proc_critical_unset.c','persistence\schtask_persist.c','persistence\schtask_persist_xml.c','persistence\schtask_persist_reg.c','persistence\svc_create_api.c','user_mgmt\user_create_net.c','user_mgmt\user_create_cmd.c','user_mgmt\user_create_ps.c')) {
    $src = Join-Path $bofsTree $c
    if (-not (Test-Path $src)) { continue }
    $base = [IO.Path]::GetFileNameWithoutExtension($c)
    $obj = Join-Path $bofsDir ($base + '.x64.o')
    & gcc -I $bofsTree -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Warning "  gcc failed on $c ... skipping (BOF optional)" }
  }
  $n = @(Get-ChildItem -Path $bofsDir -Filter '*.x64.o').Count
  Write-Host "[ok] BOF library now has $n BOF(s)" -ForegroundColor Green
}

$implantSilentExe = Join-Path $root 'target\release\ruststrike-implant-silent.exe'
Ensure-Built 'Rust implant'  $implantExe { cargo build --release -p ruststrike-implant } $root
Ensure-Built 'Rust implant (silent)' $implantSilentExe { cargo build --release -p ruststrike-implant } $root
Ensure-Built 'Go core'      $coreExe    { Push-Location (Join-Path $root 'clients\server'); go build -o $coreExe .; Pop-Location } $root
Ensure-Built 'Wails GUI'    $guiExe     { wails build } (Join-Path $root 'clients\client')
Ensure-Bofs

# --- launch -------------------------------------------------------------------
$env:RUSTSTRIKE_BOFS = if ($env:RUSTSTRIKE_BOFS) { $env:RUSTSTRIKE_BOFS } else { $bofsDir }
# SQLite DB next to the core exe; stub builder finds the implant exe.
$env:RUSTSTRIKE_DB = if ($env:RUSTSTRIKE_DB) { $env:RUSTSTRIKE_DB } elseif ($config -and $config.paths.db_path) { Resolve-ConfigPath $config.paths.db_path } else { Join-Path $root 'ruststrike.db' }
$env:RUSTSTRIKE_IMPLANT_EXE = if ($env:RUSTSTRIKE_IMPLANT_EXE) { $env:RUSTSTRIKE_IMPLANT_EXE } elseif ($config -and $config.paths.implant_exe) { Resolve-ConfigPath $config.paths.implant_exe } else { $implantExe }
$generatedAuth = $false
if (-not $env:RUSTSTRIKE_AUTH_TOKEN -and -not $configAuthToken) {
  $tokenBytes = New-Object byte[] 32
  $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
  try { $rng.GetBytes($tokenBytes) } finally { $rng.Dispose() }
  $env:RUSTSTRIKE_AUTH_TOKEN = [Convert]::ToBase64String($tokenBytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
  $generatedAuth = $true
}
if (-not $env:RUSTSTRIKE_AUTH_USERNAME -and -not $configAuthUsername) {
  $env:RUSTSTRIKE_AUTH_USERNAME = 'admin'
  $generatedAuth = $true
}
if (-not $env:RUSTSTRIKE_AUTH_PASSWORD -and -not $configAuthPassword) {
  $passBytes = New-Object byte[] 18
  $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
  try { $rng.GetBytes($passBytes) } finally { $rng.Dispose() }
  $env:RUSTSTRIKE_AUTH_PASSWORD = [Convert]::ToBase64String($passBytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
  $generatedAuth = $true
}
$effectiveUser = if ($env:RUSTSTRIKE_AUTH_USERNAME) { $env:RUSTSTRIKE_AUTH_USERNAME } else { $configAuthUsername }
$effectivePass = if ($env:RUSTSTRIKE_AUTH_PASSWORD) { $env:RUSTSTRIKE_AUTH_PASSWORD } else { $configAuthPassword }
$effectiveToken = if ($env:RUSTSTRIKE_AUTH_TOKEN) { $env:RUSTSTRIKE_AUTH_TOKEN } else { $configAuthToken }
if ($generatedAuth) {
  Write-Host "[auth] generated missing auth values for this launcher run" -ForegroundColor DarkGray
} else {
  Write-Host "[auth] using configured auth values" -ForegroundColor DarkGray
}

$jobs = New-Object System.Collections.Generic.List[object]
$implantShell = $null

function New-TitledProcess($exe, $title, $argList) {
  # Start in a new console window so each piece is independently observable/killable.
  $psi = [Diagnostics.ProcessStartInfo]::new($exe)
  if ($argList) { foreach ($a in $argList) { $psi.ArgumentList.Add($a) } }
  $psi.UseShellExecute = $true
  $psi.WorkingDirectory = $root
  $p = [Diagnostics.Process]::Start($psi)
  Write-Host "[run] $title  (pid $($p.Id))" -ForegroundColor Cyan
  return $p
}

# Core first (implant needs it listening).
$coreArgs = @($TcpPort, $HttpPort)
$coreProc = New-TitledProcess $coreExe 'Go core' $coreArgs
$jobs.Add($coreProc)

Start-Sleep -Milliseconds 800   # let the TCP listener bind before the implant dials

if (-not $NoImplant) {
  $implantProc = New-TitledProcess $implantExe 'Rust implant' @('127.0.0.1', $TcpPort)
  $jobs.Add($implantProc)
}

if (-not $NoGui) {
  Start-Sleep -Milliseconds 400   # let the implant register so the GUI sees it
  $guiProc = New-TitledProcess $guiExe 'Wails GUI' $null
  $jobs.Add($guiProc)
}

Write-Host ""
Write-Host "RustStrike is up." -ForegroundColor Green
Write-Host "  core    : http://127.0.0.1:$HttpPort  (TCP :$TcpPort for implants)"
Write-Host "  login   : $effectiveUser / $effectivePass"
Write-Host "  auth    : Authorization: Bearer $effectiveToken"
Write-Host "  REST    : GET  /api/implants   |  POST /api/bofs/ps/run?implant=1  -d `"{\`"args\`":\`"\`"}`""
Write-Host "  stop    : close this window (or Ctrl+C) ... all child processes exit."
Write-Host ""

# Keep the launcher alive until any child exits, then clean up the rest.
try {
  while ($true) {
    Start-Sleep -Seconds 1
    foreach ($p in $jobs) {
      if ($p -and $p.HasExited) {
        Write-Host "[stop] $($p.ProcessName) exited (code $($p.ExitCode))" -ForegroundColor Yellow
        throw 'child exited'
      }
    }
  }
} finally {
  Write-Host "[cleanup] stopping remaining processes..." -ForegroundColor Yellow
  foreach ($p in $jobs) {
    if ($p -and -not $p.HasExited) {
      try { $p.CloseMainWindow() | Out-Null; $p.Kill() } catch { }
    }
  }
}
