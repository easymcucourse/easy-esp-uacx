# SPDX-FileCopyrightText: 2026 easymcucourse
# SPDX-License-Identifier: MIT
param(
    [string]$IdfPath = 'C:\Espressif\frameworks\esp-idf-v5.5.1',
    [string]$Compiler = 'gcc',
    [switch]$WithoutCx31993,
    [switch]$WithoutReported,
    [switch]$WithoutEs9318,
    [switch]$WithoutCs43198,
    [switch]$WithoutCs43131
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'check_license.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'test_license.ps1')
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskComponent = Join-Path $taskRoot 'components/easy-esp-uacx'
$taskHost = Join-Path $taskComponent 'test/host'
$taskOutput = Join-Path $taskRoot 'tmp/host-tests'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
if ($WithoutCx31993) {
    $taskDefine = '/* CX31993 disabled */'
    $taskExe = Join-Path $taskOutput 'tests-generic.exe'
} else {
    $taskDefine = '#define CONFIG_EUACX_DRV_CX31993 1'
    $taskExe = Join-Path $taskOutput 'tests-cx31993.exe'
}
if (-not $WithoutReported) { $taskDefine += "`n#define CONFIG_EUACX_DRV_REPORTED 1" }
Set-Content -LiteralPath (Join-Path $taskOutput 'sdkconfig.h') -Value $taskDefine -Encoding ascii
Add-Content -LiteralPath (Join-Path $taskOutput 'sdkconfig.h') -Value '#define CONFIG_EUACX_NUM_TRANSFERS 4' -Encoding ascii
if (-not $WithoutEs9318) {
    Add-Content -LiteralPath (Join-Path $taskOutput 'sdkconfig.h') -Value '#define CONFIG_EUACX_DRV_ES9318 1' -Encoding ascii
}
if (-not $WithoutCs43131) {
    Add-Content -LiteralPath (Join-Path $taskOutput 'sdkconfig.h') -Value '#define CONFIG_EUACX_DRV_CS43131 1' -Encoding ascii
}
if (-not $WithoutCs43198) {
    Add-Content -LiteralPath (Join-Path $taskOutput 'sdkconfig.h') -Value '#define CONFIG_EUACX_DRV_CS43198 1' -Encoding ascii
}
$taskUnity = Join-Path $IdfPath 'components/unity/unity/src'
$taskArgs = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-o', $taskExe,
    "-I$taskOutput", "-I$taskHost", "-I$taskUnity",
    "-I$taskComponent/test", "-I$taskComponent/include", "-I$taskComponent/private",
    "-I$IdfPath/components/esp_common/include",
    (Join-Path $taskUnity 'unity.c'), (Join-Path $taskHost 'runner.c'))
$taskSources = @('test/test_easy_uacx.c', 'test/test_pcm_model.c', 'test/host/test_device_behavior.c', 'core/euacx_control.c', 'core/euacx_parser.c', 'core/euacx_caps.c',
    'stream/euacx_spsc.c', 'core/euacx_selector.c',
    'stream/euacx_packetizer.c', 'stream/euacx_ring.c', 'format/euacx_pcm.c',
    'format/uac2_dop.c', 'format/uac2_native_dsd.c',
    'drivers/euacx_drivers.c', 'drivers/drv_generic.c', 'quirks/quirk_table.c')
if (-not $WithoutEs9318) { $taskSources += 'quirks/quirk_es9318.c' }
if (-not $WithoutCs43198) { $taskSources += 'quirks/quirk_cs43198.c' }
if (-not $WithoutCs43131) { $taskSources += 'quirks/quirk_cs43131.c' }
if (-not $WithoutCx31993) { $taskSources += 'quirks/quirk_cx31993.c' }
foreach ($taskSource in $taskSources) { $taskArgs += Join-Path $taskComponent $taskSource }
& $Compiler @taskArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExe
exit $LASTEXITCODE
