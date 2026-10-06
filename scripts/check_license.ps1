# SPDX-FileCopyrightText: 2026 easymcucourse
# SPDX-License-Identifier: MIT
param([string]$Root = (Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $Root).Path
# Assemble forbidden strings so this checker does not match its own rules.
$taskRules = @(
    ('SPDX-License-Identifier:\s*' + 'G' + 'PL'),
    ('G' + 'PL-2\.0'), ('L' + 'GPL'),
    ('QUIRK' + '_FLAG_'), ('snd' + '_usb_'),
    ('USB' + '_MIXER_'), ('SN' + 'DRV_'),
    ('^\s*#\s*include\s*[<"]\s*' + 'linux' + '/')
)
$taskPattern = ($taskRules -join '|')
$taskFailed = $false
function Test-SourceDirectory([string]$Directory) {
    foreach ($taskItem in Get-ChildItem -LiteralPath $Directory -Force) {
        if ($taskItem.PSIsContainer) {
            if ($taskItem.Name -notin @('build', 'managed_components', '.git', '__pycache__')) {
                Test-SourceDirectory $taskItem.FullName
            }
        } elseif ($taskItem.Extension -in @('.c','.h','.cpp','.hpp','.ps1','.py','.bat','.cmake','.yml') -or
                  $taskItem.Name -in @('Kconfig','Kconfig.projbuild','CMakeLists.txt')) {
            $taskLine = 0
            foreach ($taskText in Get-Content -LiteralPath $taskItem.FullName) {
                $taskLine++
                if ($taskText -match $taskPattern) {
                    Write-Host ("License check rejected {0}:{1}" -f $taskItem.FullName, $taskLine)
                    $script:taskFailed = $true
                }
            }
        }
    }
}
foreach ($taskDir in @('components','examples','scripts')) {
    $taskPath = Join-Path $taskRoot $taskDir
    if (Test-Path -LiteralPath $taskPath) { Test-SourceDirectory $taskPath }
}
if ($taskFailed) { exit 1 }
Write-Host 'License/source checks passed.'
exit 0
