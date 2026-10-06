# SPDX-FileCopyrightText: 2026 easymcucourse
# SPDX-License-Identifier: MIT
$ErrorActionPreference = 'Stop'
$taskFixture = Join-Path ([IO.Path]::GetTempPath()) ('euacx-license-' + [guid]::NewGuid())
$taskChecker = Join-Path $PSScriptRoot 'check_license.ps1'
$taskShell = (Get-Process -Id $PID).Path
$taskSource = Join-Path $taskFixture 'components'
New-Item -ItemType Directory -Path $taskSource | Out-Null
$taskFile = Join-Path $taskSource 'fixture.c'
try {
    $taskCases = @(
        ('// SPDX-License-Identifier: ' + 'G' + 'PL-2.0'),
        ('// SPDX-License-Identifier: ' + 'L' + 'GPL-2.1'),
        ('int ' + 'QUIRK' + '_FLAG_TEST;'), ('int ' + 'snd' + '_usb_test;'),
        ('int ' + 'USB' + '_MIXER_TEST;'), ('int ' + 'SN' + 'DRV_TEST;'),
        ('#include <' + 'linux' + '/usb/audio.h>'), ('# include "' + 'linux' + '/usb/audio.h"')
    )
    foreach ($taskCase in $taskCases) {
        Set-Content -LiteralPath $taskFile -Value $taskCase
        & $taskShell -NoProfile -File $taskChecker -Root $taskFixture | Out-Null
        if ($LASTEXITCODE -eq 0) { throw 'Forbidden source fixture was accepted' }
    }
    Set-Content -LiteralPath $taskFile -Value '// SPDX-License-Identifier: MIT'
    & $taskShell -NoProfile -File $taskChecker -Root $taskFixture | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'MIT fixture was rejected' }
    Write-Host 'License checker negative fixtures passed (8 cases).'
} finally {
    # Remove only this test's file and then its empty directories.
    $taskAbsolute = [IO.Path]::GetFullPath($taskFixture)
    $taskTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $taskAbsolute.StartsWith($taskTemp, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($taskAbsolute) -notlike 'euacx-license-*') { throw 'Unexpected fixture directory' }
    Remove-Item -LiteralPath $taskFile -Force
    Remove-Item -LiteralPath $taskSource
    Remove-Item -LiteralPath $taskAbsolute
}
