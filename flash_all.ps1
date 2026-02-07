# Flash firmware to all 3 ESP32s (COM50, COM51, COM52).
# Requires Arduino CLI: https://arduino.github.io/arduino-cli/
# Install: winget install Arduino.ArduinoCLI  (or download from the link above)
# Run from project folder: .\flash_all.ps1

$ErrorActionPreference = "Stop"
$SketchDir = $PSScriptRoot
$FQBN = "esp32:esp32:esp32"   # ESP32 Dev Module; change if you use another board

# Ports: master, bridge, transponder (adjust if your assignment differs)
$Ports = @("COM50", "COM51", "COM52")

# Check arduino-cli
if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    Write-Host "arduino-cli not found. Install it:" -ForegroundColor Red
    Write-Host "  winget install Arduino.ArduinoCLI" -ForegroundColor Yellow
    Write-Host "  Or: https://arduino.github.io/arduino-cli/" -ForegroundColor Yellow
    exit 1
}

Write-Host "Building once..." -ForegroundColor Cyan
arduino-cli compile --fqbn $FQBN $SketchDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

foreach ($port in $Ports) {
    Write-Host "`nUploading to $port..." -ForegroundColor Cyan
    arduino-cli upload -p $port --fqbn $FQBN $SketchDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "`nAll 3 ESP32s flashed (COM50, COM51, COM52)." -ForegroundColor Green
