param(
    [string]$ToolchainBin = ""
)

$ErrorActionPreference = "Stop"
$firmwareRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $firmwareRoot "Build"

if (-not $ToolchainBin) {
    $preferred = "C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740\tools\bin"
    if (Test-Path (Join-Path $preferred "arm-none-eabi-gcc.exe")) {
        $ToolchainBin = $preferred
    } else {
        $candidate = Get-ChildItem "C:\ST" -Recurse -Filter "arm-none-eabi-gcc.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $candidate) {
            throw "ARM GCC bulunamadı. -ToolchainBin ile toolchain bin klasörünü verin."
        }
        $ToolchainBin = $candidate.Directory.FullName
    }
}

$gcc = Join-Path $ToolchainBin "arm-none-eabi-gcc.exe"
$objcopy = Join-Path $ToolchainBin "arm-none-eabi-objcopy.exe"
$sizeTool = Join-Path $ToolchainBin "arm-none-eabi-size.exe"
foreach ($tool in @($gcc, $objcopy, $sizeTool)) {
    if (-not (Test-Path $tool)) { throw "Araç bulunamadı: $tool" }
}

New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

$sources = @(
    "Startup\startup_stm32l152retx.s",
    "Src\main.c",
    "Src\stm32l1xx_hal_msp.c",
    "Src\stm32l1xx_it.c",
    "Src\system_stm32l1xx.c",
    "Src\syscalls.c",
    "Src\sysmem.c",
    "App\Src\autonomy.c",
    "App\Src\imu_i2c.c",
    "App\Src\ms5837.c",
    "App\Src\seastars_runtime.c",
    "Middleware\Adafruit_SH2\sh2.c",
    "Middleware\Adafruit_SH2\sh2_SensorValue.c",
    "Middleware\Adafruit_SH2\sh2_util.c",
    "Middleware\Adafruit_SH2\shtp.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_cortex.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_dma.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_flash.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_flash_ex.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_flash_ramfunc.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_gpio.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_i2c.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_pwr.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_pwr_ex.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_rcc.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_rcc_ex.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_tim.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_tim_ex.c",
    "Drivers\STM32L1xx_HAL_Driver\Src\stm32l1xx_hal_uart.c"
)

$includeDirectories = @(
    "Inc",
    "App\Inc",
    "Middleware\Adafruit_SH2",
    "Drivers\STM32L1xx_HAL_Driver\Inc",
    "Drivers\STM32L1xx_HAL_Driver\Inc\Legacy",
    "Drivers\CMSIS\Device\ST\STM32L1xx\Include",
    "Drivers\CMSIS\Include"
)

$commonFlags = @(
    "-mcpu=cortex-m3",
    "-mthumb",
    "-mfloat-abi=soft",
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-common",
    "-Wall",
    "-Wextra",
    "-Werror=implicit-function-declaration",
    "-Og",
    "-g3"
)
$defines = @("-DUSE_HAL_DRIVER", "-DSTM32L152xE")
$includes = $includeDirectories | ForEach-Object { "-I" + (Join-Path $firmwareRoot $_) }
$objects = @()

foreach ($relativeSource in $sources) {
    $source = Join-Path $firmwareRoot $relativeSource
    if (-not (Test-Path $source)) { throw "Kaynak bulunamadı: $source" }
    $objectName = ($relativeSource -replace '[\\/:]', '_') -replace '\.(c|s)$', '.o'
    $object = Join-Path $buildRoot $objectName
    $compileFlags = @($commonFlags + $defines + $includes)
    if ($source.EndsWith(".c")) { $compileFlags += "-std=gnu11" }
    if ($relativeSource.StartsWith("App\") -or $relativeSource.StartsWith("Src\")) {
        $compileFlags += "-Werror"
    } elseif ($relativeSource.StartsWith("Middleware\") -or
              $relativeSource.StartsWith("Drivers\")) {
        # Third-party sources are kept verbatim; hide only their known style warnings.
        $compileFlags += @("-Wno-unused-parameter", "-Wno-old-style-declaration", "-Wno-sign-compare")
    }
    & $gcc @compileFlags -c $source -o $object
    if ($LASTEXITCODE -ne 0) { throw "Derleme başarısız: $relativeSource" }
    $objects += $object
}

$elf = Join-Path $buildRoot "seastars_stm32l152re.elf"
$hex = Join-Path $buildRoot "seastars_stm32l152re.hex"
$bin = Join-Path $buildRoot "seastars_stm32l152re.bin"
$map = Join-Path $buildRoot "seastars_stm32l152re.map"
$linkerScript = Join-Path $firmwareRoot "STM32L152RETX_FLASH.ld"
$linkFlags = @(
    "-T$linkerScript",
    "--specs=nano.specs",
    "--specs=nosys.specs",
    "-Wl,-Map=$map",
    "-Wl,--gc-sections",
    "-Wl,--print-memory-usage",
    "-u", "_printf_float",
    "-static",
    "-lm"
)
& $gcc @commonFlags @objects @linkFlags -o $elf
if ($LASTEXITCODE -ne 0) { throw "Bağlama başarısız" }

& $objcopy -O ihex $elf $hex
if ($LASTEXITCODE -ne 0) { throw "HEX üretilemedi" }
& $objcopy -O binary -S $elf $bin
if ($LASTEXITCODE -ne 0) { throw "BIN üretilemedi" }
& $sizeTool $elf

$hash = (Get-FileHash -Algorithm SHA256 $bin).Hash
$manifest = @(
    "Firmware: Sea Stars STM32L152RE $([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss'))",
    "Target: NUCLEO-L152RE / STM32L152RETx",
    "Binary: $bin",
    "Bytes: $((Get-Item $bin).Length)",
    "SHA256: $hash",
    "Toolchain: $(& $gcc --version | Select-Object -First 1)"
)
$manifest | Set-Content -Encoding UTF8 (Join-Path $buildRoot "BUILD_INFO.txt")
$manifest | ForEach-Object { Write-Host $_ }
