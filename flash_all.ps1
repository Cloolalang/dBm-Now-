# Flash firmware to all 3 ESP32s (COM5, COM29, COM31). Run from project folder: .\flash_all.ps1
$ErrorActionPreference = "Stop"
Write-Host "Building once..." -ForegroundColor Cyan
pio run
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($env in @("esp32dev", "esp32dev_com29", "esp32dev_com31")) {
    Write-Host "`nUploading to $env..." -ForegroundColor Cyan
    pio run -t upload -e $env
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Write-Host "`nAll 3 ESP32s flashed." -ForegroundColor Green
