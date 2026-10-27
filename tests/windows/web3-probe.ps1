# CI only: exact original failing runtime and full test order, separate observer.
param([Parameter(Mandatory)][string] $Dir, [Parameter(Mandatory)][string] $Observer)
$ErrorActionPreference = 'Stop'
$script:web3Observer = (Resolve-Path -LiteralPath $Observer).Path
. "$PSScriptRoot/web3/Observer.ps1"
Web3ObserverControl
Write-Host "WEB3_DIAGNOSTIC_ONLY original-runtime=$env:SOURCE_RUN/$env:SOURCE_SHA harness=$env:GITHUB_SHA no-production-rebuild=true full-suite-order=true"
# Keep Dir exactly as supplied: the original full suite used relative dist.
# The original test54 records its deadline failure before calling the observer.
. "$PSScriptRoot/run-tests.ps1" $Dir
exit $LASTEXITCODE
