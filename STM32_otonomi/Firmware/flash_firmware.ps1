param(
    [switch]$ConfirmMotorPowerOff,
    [string]$ExpectedStlinkSerial = "066EFF565271525067121519",
    [string]$ProgrammerCli = "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
)

$ErrorActionPreference = "Stop"
$firmwareRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$hex = Join-Path $firmwareRoot "Build\seastars_stm32l152re.hex"

if (-not $ConfirmMotorPowerOff) {
    throw "Güvenlik kilidi: ESC ve step motor ana gücünü kapatın, pervaneleri sökün ve -ConfirmMotorPowerOff parametresini verin."
}
if (-not (Test-Path $ProgrammerCli)) { throw "STM32 Programmer bulunamadı: $ProgrammerCli" }
if (-not (Test-Path $hex)) { throw "Firmware HEX bulunamadı; önce build_firmware.ps1 çalıştırın." }

$probeOutput = & $ProgrammerCli -c "port=SWD" "sn=$ExpectedStlinkSerial" 2>&1
if ($LASTEXITCODE -ne 0) { throw "ST-Link bağlantısı kurulamadı.`n$($probeOutput -join [Environment]::NewLine)" }
$probeText = $probeOutput -join "`n"
if ($probeText -notmatch "Device ID\s*:\s*0x437") {
    throw "Beklenen STM32L152RE (Device ID 0x437) bağlı değil; yazma iptal edildi."
}

Write-Host "Doğrulandı: ST-Link $ExpectedStlinkSerial / STM32L152RE (0x437)"
Write-Host "Yazılıyor ve doğrulanıyor: $hex"
& $ProgrammerCli -c "port=SWD" "sn=$ExpectedStlinkSerial" -d $hex -v -rst
if ($LASTEXITCODE -ne 0) { throw "Firmware yazma/doğrulama başarısız." }
Write-Host "Firmware yazıldı, doğrulandı ve kart resetlendi."
