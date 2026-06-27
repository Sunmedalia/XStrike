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
# Override ports / core URL via the usual env vars (RUSTSTRIKE_BOFS,
# RUSTSTRIKE_CORE) -- they're forwarded to the child processes.
#
# Prereqs (built if missing):
#   - target\release\ruststrike-implant.exe   (cargo build --release)
#   - clients\go-server\go-server.exe         (go build)
#   - clients\wails-gui\build\bin\wails-gui.exe (wails build)
#   - clients\go-server\bofs\*.x64.o          (gcc -c examples/*.c + copy)

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
$coreExe    = Join-Path $root 'clients\go-server\go-server.exe'
$guiExe     = Join-Path $root 'clients\wails-gui\build\bin\wails-gui.exe'
$bofsDir    = Join-Path $root 'clients\go-server\bofs'

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
  $expected = @('hello','cmd_exec','powershell_exec','winapi_exec','ps','proc_list','proc_kill','ls','file_list','download','file_download','screenshot','upload','shellcode_exec','shellcode_exec_nt')
  $have = @(Get-ChildItem -Path $bofsDir -Filter '*.x64.o' -ErrorAction SilentlyContinue | ForEach-Object { $_.BaseName })
  $missing = $expected | Where-Object { $have -notcontains $_ }
  if ($missing.Count -eq 0) { Write-Host "[ok] BOF library has $($have.Count) BOF(s)" -ForegroundColor DarkGray; return }
  Write-Host "[build] staging BOFs into $bofsDir (missing: $($missing -join ', '))..." -ForegroundColor Yellow
  $examples = Join-Path $root 'examples'
  foreach ($c in @('hello.c','cmd_exec.c','powershell_exec.c','winapi_exec.c','ps.c','proc_list.c','proc_kill.c','ls.c','file_list.c','download.c','file_download.c','screenshot.c','upload.c','shellcode_exec.c','shellcode_exec_nt.c')) {
    $src = Join-Path $examples $c
    $obj = Join-Path $examples ([IO.Path]::GetFileNameWithoutExtension($c) + '.x64.o')
    if (-not (Test-Path $obj)) {
      & gcc -c $src -o $obj
      if ($LASTEXITCODE -ne 0) { Write-Warning "  gcc failed on $c ... skipping (BOF optional)" }
    }
    if (Test-Path $obj) { Copy-Item $obj $bofsDir -Force }
  }
  $n = @(Get-ChildItem -Path $bofsDir -Filter '*.x64.o').Count
  Write-Host "[ok] BOF library now has $n BOF(s)" -ForegroundColor Green
}

Ensure-Built 'Rust implant'  $implantExe { cargo build --release -p ruststrike-implant } $root
Ensure-Built 'Go core'      $coreExe    { Push-Location (Join-Path $root 'clients\go-server'); go build -o $coreExe .; Pop-Location } $root
Ensure-Built 'Wails GUI'    $guiExe     { wails build } (Join-Path $root 'clients\wails-gui')
Ensure-Bofs

# --- launch -------------------------------------------------------------------
$env:RUSTSTRIKE_BOFS = $bofsDir

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
