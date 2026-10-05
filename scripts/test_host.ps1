param(
    [string]$IdfPath = 'C:\Espressif\frameworks\esp-idf-v5.5.1',
    [string]$Compiler = 'gcc',
    [switch]$WithoutCx31993
)
$ErrorActionPreference = 'Stop'
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
Set-Content -LiteralPath (Join-Path $taskOutput 'sdkconfig.h') -Value $taskDefine -Encoding ascii
$taskUnity = Join-Path $IdfPath 'components/unity/unity/src'
$taskArgs = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-o', $taskExe,
    "-I$taskOutput", "-I$taskHost", "-I$taskUnity",
    "-I$taskComponent/include", "-I$taskComponent/private",
    "-I$IdfPath/components/esp_common/include",
    (Join-Path $taskUnity 'unity.c'), (Join-Path $taskHost 'runner.c'))
$taskSources = @('test/test_easy_uacx.c', 'test/test_pcm_model.c', 'core/euacx_parser.c', 'core/euacx_caps.c',
    'stream/euacx_spsc.c', 'core/euacx_selector.c',
    'stream/euacx_packetizer.c', 'stream/euacx_ring.c', 'format/euacx_pcm.c',
    'format/uac2_dop.c', 'format/uac2_native_dsd.c',
    'drivers/euacx_drivers.c', 'drivers/drv_generic.c')
if (-not $WithoutCx31993) { $taskSources += 'drivers/drv_cx31993.c' }
foreach ($taskSource in $taskSources) { $taskArgs += Join-Path $taskComponent $taskSource }
& $Compiler @taskArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExe
exit $LASTEXITCODE
