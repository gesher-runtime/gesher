# Run under Windows PowerShell 5.1 in the package context being evaluated.
# This read-only diagnostic never supplies an entitlement to the runtime.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Inspect')]
    [switch]$Inspect,

    [Parameter(Mandatory = $true, ParameterSetName = 'Verify')]
    [ValidateNotNullOrEmpty()]
    [string]$ExpectedPackageFamily,

    [Parameter(Mandatory = $true, ParameterSetName = 'Verify')]
    [ValidateNotNullOrEmpty()]
    [string]$ExpectedAddOnSku,

    [Parameter(Mandatory = $true, ParameterSetName = 'Verify')]
    [ValidateNotNullOrEmpty()]
    [string]$ExpectedOfferToken,

    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'GesherStoreEntitlement.psm1')
$result = Get-GesherStoreDiagnostic @PSBoundParameters
$result | ConvertTo-Json -Compress
switch ($result.Outcome) {
    'InspectionOnly' { exit 0 }
    'ExpectedAddOnPresent' { exit 0 }
    'PackageIdentityMismatch' { exit 2 }
    'ExpectedAddOnAbsent' { exit 2 }
    default { exit 3 }
}
