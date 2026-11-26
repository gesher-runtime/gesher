# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
# Exercise the production language merge without changing HKCU.
param([string] $UiPath = (Join-Path $PSScriptRoot '../../../tools/package/gesher-ui.ps1'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
. $UiPath
$zero = [char]0
$english = 'en-US' + $zero + $zero
$hebrew = 'he-IL' + $zero + $zero
$cases = @(
    @{ Profile = [string[]]@('he-IL', 'en-US'); Display = $english; Expected = 'he-IL,en-US' },
    @{ Profile = [string[]]@('en-US', 'he-IL'); Display = $hebrew; Expected = 'en-US,he-IL' },
    @{ Profile = 'he-IL'; Display = $english; Expected = 'he-IL,en-US' },
    @{ Profile = ('fr-FR' + $zero + 'he-IL'); Display = $english; Expected = 'fr-FR,en-US' },
    @{ Profile = [string[]]@('he-IL', 'HE-il', 'en-US'); Display = ('EN-us' + $zero + 'fr-FR' + $zero + $zero + 'he'); Expected = 'he-IL,en-US,fr-FR' },
    @{ Profile = [string[]]@('fr-FR', '', 'he-IL'); Display = $english; Expected = 'fr-FR,en-US' },
    @{ Profile = [string[]]@('fr-FR', $null, 'he-IL'); Display = $english; Expected = 'fr-FR,en-US' },
    @{ Profile = [string[]]@(' he-IL ', 'bad,tag', 'fr--FR', 'en-US'); Display = $english; Expected = 'he-IL,en-US' },
    @{ Profile = [string[]]@('fr-FR'); Display = $hebrew; Expected = 'fr-FR,he-IL' },
    @{ Profile = [string[]]@(); Display = $hebrew; Expected = 'he-IL' },
    @{ Profile = $null; Display = ('fr-FR' + $zero + $zero + 'he-IL'); Expected = 'fr-FR' },
    @{ Profile = [byte[]]@(1, 2); Display = $english; Expected = 'en-US' },
    @{ Profile = 'he-IL'; Display = $null; Expected = 'he-IL' },
    @{ Profile = $null; Display = 'he-IL'; Expected = 'he-IL' },
    @{ Profile = $null; Display = $null; Expected = 'en' }
)
$checks = 0
foreach ($case in $cases) {
    $actual = Merge-GesherUiLanguages $case.Profile $case.Display
    if ($actual -cne $case.Expected) { throw "Preferred language order: expected '$($case.Expected)', got '$actual'" }
    $checks++
}
$longProfile = [string[]](@(0..299 | ForEach-Object { 'qaa-x-{0:D4}' -f $_ }) + @('he-IL'))
$expected = (@($longProfile) + @('en-US')) -join ','
if ($expected.Length -le 1024 -or (Merge-GesherUiLanguages $longProfile $english) -cne $expected) {
    throw 'Preferred language list was truncated or reordered'
}
$checks++
Write-Host "PASS: $checks production preferred-language checks without changing user settings"
