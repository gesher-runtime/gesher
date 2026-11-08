$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $repoRoot 'tools/package/gesher-ui.ps1')
$cases = @{
    'he-IL-u-ca-hebrew,en-US' = 'he'
    ' HE_il , en-US' = 'he'
    'de-DE,iw-IL,en-US' = 'he'
    'en-GB,he-IL' = 'en'
    'ar-SA,fr-FR' = 'en'
    'hebrew,xx' = 'en'
    '' = 'en'
}
foreach ($case in $cases.GetEnumerator()) {
    if ((Get-GesherUiLanguage $case.Key) -ne $case.Value) { throw "Language selection: $($case.Key)" }
}
$script:GesherUiLanguage = 'he'
$path = 'C:\שלום\{1}.apk'
if ((Get-GesherText 'microg_missing' @($path)) -ne "קובץ ההרחבה אינו קיים: $path") { throw 'Literal placeholder in path changed' }
if ((Get-GesherText 'starting') -ne 'מפעיל…') { throw 'Hebrew string missing' }
$script:GesherUiLanguage = 'en'
if ((Get-GesherText 'starting') -ne 'Starting…') { throw 'English string missing' }
foreach ($file in 'gesher-ui.ps1', 'gesher-app.ps1', 'gesher-app-microg.ps1', 'gesher-control-panel.ps1') {
    $tokens = $null; $parseErrors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repoRoot "tools/package/$file"), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
}
'PASS: PowerShell UI translations, language selection, literal paths and entry point syntax'
