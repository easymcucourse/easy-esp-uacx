param(
    [ValidateSet('p4', 's3')][string]$Board = 'p4',
    [string]$Port = '',
    [ValidateSet('Default', 'Matrix', 'Soak')][string]$Profile = 'Default'
)
$ErrorActionPreference = 'Stop'
$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:IDF_PATH = 'C:\Espressif\frameworks\esp-idf-v5.5.1'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\python_env\idf5.5_py3.11_env'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011'
$env:PATH = "$env:IDF_PYTHON_ENV_PATH\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;$env:PATH"
$example = Join-Path $PSScriptRoot "..\examples\$Board"
$idfArgs = @('-C', $example)
if ($Profile -ne 'Default') {
    $taskProfile = $Profile.ToLowerInvariant()
    $taskBuild = Join-Path $example "build/$taskProfile"
    New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
    $taskDefaults = (Join-Path $example 'sdkconfig.defaults') + ';' +
        (Join-Path $PSScriptRoot "../examples/common/sdkconfig.$taskProfile.defaults")
    $idfArgs += @('-B', $taskBuild, '-D', "SDKCONFIG=$(Join-Path $taskBuild 'sdkconfig')",
        '-D', "SDKCONFIG_DEFAULTS=$taskDefaults")
}
if ($Port) { $idfArgs += @('-p', $Port) }
$idfArgs += 'build'
if ($Port) { $idfArgs += 'flash' }
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" @idfArgs
exit $LASTEXITCODE
