# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0

# Populate together from Partner Center when a real release catalog is assigned.
# Installed callers must use the packaged catalog. The diagnostic catalog is
# only for comparing outcomes in test installation contexts.
$script:ReleaseCatalog = $null
$script:PendingPurchase = $null

function Get-GesherStorePackageFamily {
    try {
        [Windows.ApplicationModel.Package,Windows.ApplicationModel,ContentType=WindowsRuntime]::Current.Id.FamilyName
    } catch {
        # APPMODEL_ERROR_NO_PACKAGE denotes an unpackaged process.
        if ($_.Exception.GetBaseException().HResult -ne -2147009196) { throw }
    }
}

function Convert-GesherStoreOperation {
    param($Operation, [type] $ResultType)

    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
        $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    }
    $asTask.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
}

function Get-GesherStoreReleaseCatalog {
    $catalog = $script:ReleaseCatalog
    if ($catalog -and ![string]::IsNullOrWhiteSpace($catalog.PackageFamily) -and
        ![string]::IsNullOrWhiteSpace($catalog.AddOnSku) -and ![string]::IsNullOrWhiteSpace($catalog.OfferToken)) {
        $catalog
    }
}

function Test-GesherStoreLicenseIdentity {
    param($License, $Catalog)

    [string]::Equals($License.SkuStoreId, $Catalog.AddOnSku, [StringComparison]::Ordinal) -and
        [string]::Equals($License.InAppOfferToken, $Catalog.OfferToken, [StringComparison]::Ordinal)
}

function Read-GesherStoreLicense {
    param(
        $Catalog,
        [int] $TimeoutSeconds
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

    try {
        if ($PSVersionTable.PSEdition -ne 'Desktop') {
            throw [System.PlatformNotSupportedException]::new('Use Windows PowerShell 5.1.')
        }

        $result.Stage = 'PackageIdentity'
        $packageFamily = Get-GesherStorePackageFamily
        $result.HasPackageIdentity = -not [string]::IsNullOrEmpty($packageFamily)
        if ($Catalog) {
            $result.PackageFamilyMatches = [string]::Equals($packageFamily, $Catalog.PackageFamily,
                [StringComparison]::OrdinalIgnoreCase)
            if (!$result.PackageFamilyMatches) {
                $result.Outcome = 'PackageIdentityMismatch'
                return [pscustomobject] $result
            }
        }

        $result.Stage = 'ReadLicense'
        $context = [Windows.Services.Store.StoreContext,Windows.Services.Store,ContentType=WindowsRuntime]::GetDefault()
        $licenseType = [Windows.Services.Store.StoreAppLicense,Windows.Services.Store,ContentType=WindowsRuntime]
        $task = Convert-GesherStoreOperation -Operation $context.GetAppLicenseAsync() -ResultType $licenseType
        if (-not $task.Wait([TimeSpan]::FromSeconds($TimeoutSeconds))) {
            throw [System.TimeoutException]::new('Timed out querying the Store license.')
        }
        $license = $task.GetAwaiter().GetResult()
        if ($null -eq $license) { throw [System.InvalidOperationException]::new('Store returned no license.') }
        $result.BaseLicenseActive = $license.IsActive
        if (!$Catalog) {
            $result.Outcome = 'InspectionOnly'
            return [pscustomobject] $result
        }

        $result.Stage = 'CheckAddOn'
        # PowerShell sees the WinRT map as a COM object. Use its projected
        # IReadOnlyDictionary interface to look up the configured add-on SKU.
        $map = $license.AddOnLicenses
        if ($null -eq $map) { throw [System.InvalidOperationException]::new('Store returned no add-on map.') }
        $mapType = $licenseType.GetProperty('AddOnLicenses').PropertyType
        $result.HasExpectedAddOn = $false
        if ($mapType.GetMethod('ContainsKey').Invoke($map, @($Catalog.AddOnSku))) {
            $addOn = $mapType.GetProperty('Item').GetValue($map, @($Catalog.AddOnSku))
            if ($null -eq $addOn) { throw [System.InvalidOperationException]::new('Store returned no add-on license for the selected SKU.') }
            $result.HasExpectedAddOn = Test-GesherStoreLicenseIdentity -License $addOn -Catalog $Catalog
            if (!$result.HasExpectedAddOn) {
                $result.Outcome = 'AddOnIdentityMismatch'
                return [pscustomobject] $result
            }
        }
        # The map contains only currently valid durables. StoreLicense.IsActive
        # is reserved. A base app license does not grant the add-on.
        $result.Outcome = if ($result.HasExpectedAddOn) { 'ExpectedAddOnPresent' } else { 'ExpectedAddOnAbsent' }
    } catch {
        $result.Outcome = 'Unavailable'
        $result.HResult = '0x{0:X8}' -f $_.Exception.GetBaseException().HResult
    }
    [pscustomobject] $result
}

function Get-GesherStoreDiagnostic {
    [CmdletBinding(DefaultParameterSetName = 'Inspect')]
    param(
        [Parameter(ParameterSetName = 'Inspect')] [switch] $Inspect,
        [Parameter(Mandatory, ParameterSetName = 'Verify')] [ValidateNotNullOrEmpty()] [string] $ExpectedPackageFamily,
        [Parameter(Mandatory, ParameterSetName = 'Verify')] [ValidateNotNullOrEmpty()] [string] $ExpectedAddOnSku,
        [Parameter(Mandatory, ParameterSetName = 'Verify')] [ValidateNotNullOrEmpty()] [string] $ExpectedOfferToken,
        [ValidateRange(1, 120)] [int] $TimeoutSeconds = 15
    )
    $catalog = $null
    if ($PSCmdlet.ParameterSetName -eq 'Verify') {
        $catalog = @{ PackageFamily = $ExpectedPackageFamily; AddOnSku = $ExpectedAddOnSku; OfferToken = $ExpectedOfferToken }
    }
    Read-GesherStoreLicense -Catalog $catalog -TimeoutSeconds $TimeoutSeconds
}

function Get-GesherStoreEntitlement {
    [CmdletBinding()]
    param([ValidateRange(1, 120)] [int] $TimeoutSeconds = 15)

    $catalog = Get-GesherStoreReleaseCatalog
    if (!$catalog) {
        return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'CatalogUnconfigured'; HResult = $null }
    }
    $query = Read-GesherStoreLicense -Catalog $catalog -TimeoutSeconds $TimeoutSeconds
    $status = switch ($query.Outcome) {
        'ExpectedAddOnPresent' { 'Active' }
        'ExpectedAddOnAbsent' { 'Inactive' }
        default { 'Unavailable' }
    }
    [pscustomobject] @{ Status = $status; Reason = $query.Outcome; HResult = $query.HResult }
}

function Find-GesherStorePurchaseSku {
    param($Product, $Catalog)

    if (!$Product -or $Product.ProductKind -cne 'Durable' -or
        $Product.StoreId -cne $Catalog.ProductStoreId -or $Product.InAppOfferToken -cne $Catalog.OfferToken) {
        return
    }
    $skus = $Product.Skus
    $productType = [Windows.Services.Store.StoreProduct,Windows.Services.Store,ContentType=WindowsRuntime]
    if ($productType.IsInstanceOfType($Product) -and $null -ne $skus) {
        # Read the projected vector explicitly in Windows PowerShell 5.1.
        $listType = $productType.GetProperty('Skus').PropertyType
        $collectionType = $listType.GetInterfaces() | Where-Object { $_.Name -like 'IReadOnlyCollection*' }
        $count = $collectionType.GetProperty('Count').GetValue($skus)
        $item = $listType.GetProperty('Item')
        $skus = for ($i = 0; $i -lt $count; $i++) { $item.GetValue($skus, @($i)) }
    }
    $matching = @($skus | Where-Object {
        $_.StoreId -ceq $Catalog.AddOnSku -and $_.IsTrial -eq $false -and $_.IsSubscription -eq $false
    })
    if ($matching.Count -eq 1) { $matching[0] }
}

# Query the offered catalog on a worker, keeping StoreProduct and its context
# together for the dispatcher. No purchase window is opened by this query.
function Get-GesherStorePurchaseOffer {
    [CmdletBinding()]
    param([ValidateRange(1, 120)] [int] $TimeoutSeconds = 15)

    $ErrorActionPreference = 'Stop'
    $catalog = Get-GesherStoreReleaseCatalog
    if (!$catalog -or [string]::IsNullOrWhiteSpace($catalog.ProductStoreId)) {
        return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'CatalogUnconfigured' }
    }
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'UnsupportedPowerShell' }
    }
    try {
        if (![string]::Equals((Get-GesherStorePackageFamily), $catalog.PackageFamily,
                [StringComparison]::OrdinalIgnoreCase)) {
            return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'PackageIdentityMismatch' }
        }
        $context = [Windows.Services.Store.StoreContext,Windows.Services.Store,ContentType=WindowsRuntime]::GetDefault()
        $resultType = [Windows.Services.Store.StoreProductQueryResult,Windows.Services.Store,ContentType=WindowsRuntime]
        $operation = $context.GetStoreProductsAsync([string[]] @('Durable'), [string[]] @($catalog.ProductStoreId))
        $task = Convert-GesherStoreOperation -Operation $operation -ResultType $resultType
        if (!$task.Wait([TimeSpan]::FromSeconds($TimeoutSeconds))) {
            throw [System.TimeoutException]::new('Timed out querying the Store offer.')
        }
        $result = $task.GetAwaiter().GetResult()
        if ($result.ExtendedError) { throw $result.ExtendedError }
        $map = $result.Products
        $mapType = $resultType.GetProperty('Products').PropertyType
        if ($map -and $mapType.GetMethod('ContainsKey').Invoke($map, @($catalog.ProductStoreId))) {
            $product = $mapType.GetProperty('Item').GetValue($map, @($catalog.ProductStoreId))
            if (Find-GesherStorePurchaseSku -Product $product -Catalog $catalog) {
                return [pscustomobject] @{ Status = 'Ready'; Context = $context; Product = $product }
            }
        }
        [pscustomobject] @{ Status = 'Unavailable'; Reason = 'ExpectedOfferAbsent' }
    } catch {
        [pscustomobject] @{ Status = 'Unavailable'; Reason = 'StoreOfferQueryFailed'
            HResult = '0x{0:X8}' -f $_.Exception.GetBaseException().HResult }
    }
}

function Start-GesherStorePurchase {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [IntPtr] $OwnerWindow,
        $Offer
    )

    $ErrorActionPreference = 'Stop'
    $catalog = Get-GesherStoreReleaseCatalog
    if (!$catalog -or [string]::IsNullOrWhiteSpace($catalog.ProductStoreId)) {
        return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'CatalogUnconfigured' }
    }
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'UnsupportedPowerShell' }
    }
    if ($script:PendingPurchase -and !$script:PendingPurchase.IsCompleted) {
        return [pscustomobject] @{ Status = 'Busy'; Reason = 'PurchaseInProgress' }
    }

    try {
        if (![string]::Equals((Get-GesherStorePackageFamily), $catalog.PackageFamily,
                [StringComparison]::OrdinalIgnoreCase)) {
            return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'PackageIdentityMismatch' }
        }
        $contextType = [Windows.Services.Store.StoreContext,Windows.Services.Store,ContentType=WindowsRuntime]
        $productType = [Windows.Services.Store.StoreProduct,Windows.Services.Store,ContentType=WindowsRuntime]
        if (!$Offer -or $Offer.Status -ne 'Ready' -or !$contextType.IsInstanceOfType($Offer.Context) -or
            !$productType.IsInstanceOfType($Offer.Product)) {
            return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'ExpectedOfferAbsent' }
        }
        $sku = Find-GesherStorePurchaseSku -Product $Offer.Product -Catalog $catalog
        if (!$sku) { return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'ExpectedOfferAbsent' } }
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        try {
            $principal = [Security.Principal.WindowsPrincipal]::new($identity)
            if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
                return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'ElevatedProcess' }
            }
        } finally {
            $identity.Dispose()
        }
        if (!('Gesher.Store.StoreWindowOwner' -as [type])) {
            Add-Type -Path (Join-Path $PSScriptRoot 'StoreWindowOwner.cs')
        }
        [Gesher.Store.StoreWindowOwner]::Attach($Offer.Context, $OwnerWindow)
        $resultType = [Windows.Services.Store.StorePurchaseResult,Windows.Services.Store,ContentType=WindowsRuntime]
        $script:PendingPurchase = Convert-GesherStoreOperation -Operation $sku.RequestPurchaseAsync() -ResultType $resultType
        [pscustomobject] @{ Status = 'Pending'; Operation = $script:PendingPurchase }
    } catch {
        [pscustomobject] @{ Status = 'Unavailable'; Reason = 'StorePurchaseFailed'
            HResult = '0x{0:X8}' -f $_.Exception.GetBaseException().HResult }
    }
}

# A caller polls IsCompleted instead of blocking the UI. Run completion,
# which can query the Store again, on a worker in the same Windows context.
function Resolve-GesherStorePurchaseStatus {
    param([string] $Status, [int] $TimeoutSeconds)

    switch ($Status) {
        { $_ -in 'Succeeded', 'AlreadyPurchased' } {
            $entitlement = Get-GesherStoreEntitlement -TimeoutSeconds $TimeoutSeconds
            $outcome = if ($entitlement.Status -eq 'Active') { 'Verified' } else { 'PendingVerification' }
            return [pscustomobject] @{ Status = $outcome; StoreStatus = $Status; Entitlement = $entitlement }
        }
        { $_ -in 'NotPurchased', 'NetworkError', 'ServerError' } {
            return [pscustomobject] @{ Status = $Status }
        }
        default { return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'UnknownPurchaseResult' } }
    }
}

function Complete-GesherStorePurchase {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [Threading.Tasks.Task] $Operation,
        [ValidateRange(1, 120)] [int] $TimeoutSeconds = 15
    )

    $ErrorActionPreference = 'Stop'
    if (!$Operation.IsCompleted) { return [pscustomobject] @{ Status = 'Pending' } }
    try {
        $purchase = $Operation.GetAwaiter().GetResult()
        $resultType = [Windows.Services.Store.StorePurchaseResult,Windows.Services.Store,ContentType=WindowsRuntime]
        if (!$resultType.IsInstanceOfType($purchase)) {
            return [pscustomobject] @{ Status = 'Unavailable'; Reason = 'UnknownPurchaseResult' }
        }
        Resolve-GesherStorePurchaseStatus -Status ([string] $purchase.Status) -TimeoutSeconds $TimeoutSeconds
    } catch {
        [pscustomobject] @{ Status = 'Unavailable'; Reason = 'StorePurchaseFailed'
            HResult = '0x{0:X8}' -f $_.Exception.GetBaseException().HResult }
    }
}

Export-ModuleMember -Function Get-GesherStoreDiagnostic, Get-GesherStoreEntitlement, Get-GesherStorePurchaseOffer, Start-GesherStorePurchase, Complete-GesherStorePurchase
