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
$result = [ordered]@{
    Outcome = 'Unavailable'
    Stage = 'PowerShell'
    HasPackageIdentity = $null
    PackageFamilyMatches = $null
    BaseLicenseActive = $null
    HasExpectedAddOn = $null
    HResult = $null
}
$exitCode = 3

try {
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        throw [System.PlatformNotSupportedException]::new('Use Windows PowerShell 5.1.')
    }

    $result.Stage = 'PackageIdentity'
    $packageFamily = $null
    try {
        $packageFamily = [Windows.ApplicationModel.Package,Windows.ApplicationModel,ContentType=WindowsRuntime]::Current.Id.FamilyName
    } catch {
        # APPMODEL_ERROR_NO_PACKAGE is the expected result for an unpackaged process.
        if ($_.Exception.GetBaseException().HResult -ne -2147009196) { throw }
    }
    $result.HasPackageIdentity = -not [string]::IsNullOrEmpty($packageFamily)
    if ($PSCmdlet.ParameterSetName -eq 'Verify') {
        $result.PackageFamilyMatches = [string]::Equals($packageFamily, $ExpectedPackageFamily,
            [StringComparison]::OrdinalIgnoreCase)
    }

    if ($result.PackageFamilyMatches -eq $false) {
        $result.Outcome = 'PackageIdentityMismatch'
        $exitCode = 2
    } else {
        $result.Stage = 'ReadLicense'
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $context = [Windows.Services.Store.StoreContext,Windows.Services.Store,ContentType=WindowsRuntime]::GetDefault()
        $licenseType = [Windows.Services.Store.StoreAppLicense,Windows.Services.Store,ContentType=WindowsRuntime]
        $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
            $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        }
        $operation = $context.GetAppLicenseAsync()
        $task = $asTask.MakeGenericMethod($licenseType).Invoke($null, @($operation))
        if (-not $task.Wait([TimeSpan]::FromSeconds($TimeoutSeconds))) {
            throw [System.TimeoutException]::new('Timed out querying the Store license.')
        }
        $license = $task.GetAwaiter().GetResult()
        $result.BaseLicenseActive = $license.IsActive

        if ($PSCmdlet.ParameterSetName -eq 'Inspect') {
            $result.Outcome = 'InspectionOnly'
            $exitCode = 0
        } else {
            $result.Stage = 'CheckAddOn'
            # PowerShell sees the projected WinRT map as a COM object. Use the
            # projected IReadOnlyDictionary interface for a single SKU lookup.
            $addOnMap = $license.AddOnLicenses
            $mapType = $licenseType.GetProperty('AddOnLicenses').PropertyType
            $result.HasExpectedAddOn = $false
            if ($null -ne $addOnMap -and $mapType.GetMethod('ContainsKey').Invoke($addOnMap, @($ExpectedAddOnSku))) {
                $addOn = $mapType.GetProperty('Item').GetValue($addOnMap, @($ExpectedAddOnSku))
                $result.HasExpectedAddOn = $null -ne $addOn -and
                    [string]::Equals($addOn.SkuStoreId, $ExpectedAddOnSku, [StringComparison]::Ordinal) -and
                    [string]::Equals($addOn.InAppOfferToken, $ExpectedOfferToken, [StringComparison]::Ordinal)
            }
            # Microsoft documents that this map contains only currently valid
            # add-ons. StoreLicense.IsActive is reserved and always true.
            if ($result.HasExpectedAddOn) {
                $result.Outcome = 'ExpectedAddOnPresent'
                $exitCode = 0
            } else {
                $result.Outcome = 'ExpectedAddOnAbsent'
                $exitCode = 2
            }
        }
    }
} catch {
    $failure = $_.Exception.GetBaseException()
    $result.Outcome = 'Unavailable'
    $result.HResult = '0x{0:X8}' -f $failure.HResult
}

$result | ConvertTo-Json -Compress
exit $exitCode
