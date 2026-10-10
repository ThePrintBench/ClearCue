<#
  Production flasher: plug boards in one after another; each is erased, flashed with the
  merged factory image, and logged (MAC, setup-AP name, firmware version) to a CSV.

  Usage (from the project folder):
    pio run -e clearcue                     # builds + writes web/firmware/*-factory.bin
    powershell -ExecutionPolicy Bypass -File tools\batch_flash.ps1
  Options:
    -Firmware <path>   default: the factory image matching web\manifest.json (current build)
    -Log <path>        default: tools\flash_log.csv
    -Baud <n>          default: 921600 (native USB ignores it anyway)
    -NoErase           keep NVS (only for re-flashing your own configured boards)
#>
param(
  [string]$Firmware = "",
  [string]$Log = "",
  [int]$Baud = 921600,
  [switch]$NoErase
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $root "web\manifest.json"
if (-not (Test-Path $manifestPath)) { throw "No web\manifest.json. Run: pio run -e clearcue" }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
if (-not $Firmware) {
  # Versioned file names: flash exactly the image the current manifest names.
  # The manifest lists separate parts for the web flasher; production uses the merged factory image.
  $appPart = ($manifest.builds[0].parts | Where-Object { $_.path -like '*-app.bin' } | Select-Object -First 1).path
  if (-not $appPart) { throw "No app part in web\manifest.json. Rebuild: pio run -e clearcue" }
  $Firmware = Join-Path (Join-Path $root "web") (($appPart -replace '-app\.bin$', '-factory.bin') -replace '/', '\')
}
if (-not (Test-Path $Firmware)) { throw "Factory image not found: $Firmware. Run: pio run -e clearcue" }
if (-not $Log) { $Log = Join-Path $PSScriptRoot "flash_log.csv" }

$product  = $manifest.name
$version  = $manifest.version

# esptool from the pioarduino venv, falling back to whatever is on PATH.
$esptool = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\esptool.exe"
if (-not (Test-Path $esptool)) {
  $cmd = Get-Command esptool -ErrorAction SilentlyContinue
  if ($cmd) { $esptool = $cmd.Source } else { throw "esptool not found (build once with pioarduino, or pip install esptool)" }
}

if (-not (Test-Path $Log)) { "timestamp,port,mac,setup_ap,product,version,result" | Out-File $Log -Encoding utf8 }

function Get-Ports { [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique }

Write-Host "$product $version  <-  $Firmware" -ForegroundColor Cyan
Write-Host "Logging to $Log. Ctrl+C to stop.`n"
$count = 0
while ($true) {
  $before = @(Get-Ports)
  Write-Host "Plug in the next board..." -NoNewline
  do { Start-Sleep -Milliseconds 400; $new = @(Get-Ports | Where-Object { $before -notcontains $_ }) } while ($new.Count -eq 0)
  $port = $new[0]
  Write-Host " $port"
  Start-Sleep -Milliseconds 800   # let USB-CDC settle

  $eargs = @("--chip", "esp32c3", "--port", $port, "--baud", "$Baud", "write-flash")
  if (-not $NoErase) { $eargs += "--erase-all" }
  $eargs += @("0x0", $Firmware)
  $out = & $esptool @eargs 2>&1 | ForEach-Object { "$_"; Write-Host "    $_" -ForegroundColor DarkGray }
  $ok  = $LASTEXITCODE -eq 0
  $mac = ($out | Select-String -Pattern "MAC:\s*([0-9a-fA-F:]{17})" | Select-Object -First 1).Matches.Groups[1].Value
  $ap  = if ($mac) { "$product-" + ($mac -replace ":", "").Substring(8).ToUpper() } else { "" }
  $ts  = (Get-Date).ToString("s")
  "$ts,$port,$mac,$ap,$product,$version,$(if ($ok) {'ok'} else {'FAIL'})" | Out-File $Log -Append -Encoding utf8

  if ($ok) {
    $count++
    Write-Host ("  OK  #{0}  MAC {1}  label: {2}" -f $count, $mac, $ap) -ForegroundColor Green
  } else {
    Write-Host "  FAILED. Last output:" -ForegroundColor Red
    $out | Select-Object -Last 8 | ForEach-Object { Write-Host "    $_" }
    Write-Host "  Tip: hold BOOT while plugging in to force download mode."
  }
  Write-Host "  Unplug it..." -NoNewline
  do { Start-Sleep -Milliseconds 400 } while ((Get-Ports) -contains $port)
  Write-Host " done.`n"
}
