# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0

$ErrorActionPreference = 'Stop'
$module = Import-Module (Join-Path $PSScriptRoot 'GesherStoreEntitlement.psm1') -PassThru

# The diagnostic's test identity cannot select the catalog used by the provider.
$diagnostic = Get-GesherStoreDiagnostic -ExpectedPackageFamily 'Gesher.Invalid__test' -ExpectedAddOnSku 'sku' -ExpectedOfferToken 'token'
if ($diagnostic.Outcome -notin 'PackageIdentityMismatch', 'Unavailable') {
    throw "Unexpected diagnostic outcome: $($diagnostic.Outcome)"
}
$release = Get-GesherStoreEntitlement
if ($release.Status -ne 'Unavailable' -or $release.Reason -ne 'CatalogUnconfigured') {
    throw 'The unconfigured release must report unavailable entitlement'
}
$start = Start-GesherStorePurchase -OwnerWindow ([IntPtr]::Zero)
if ($start.Status -ne 'Unavailable' -or $start.Reason -ne 'CatalogUnconfigured') {
    throw 'The unconfigured release must not start a purchase'
}
$offer = Get-GesherStorePurchaseOffer
if ($offer.Status -ne 'Unavailable' -or $offer.Reason -ne 'CatalogUnconfigured') {
    throw 'The unconfigured release must not query a purchase offer'
}

# These records exercise the predicate only, outside the Windows API query.
& $module {
    $catalog = @{ ProductStoreId = 'SKU'; AddOnSku = 'SKU/0010'; OfferToken = 'upgrade' }
    $match = @{ SkuStoreId = 'SKU/0010'; InAppOfferToken = 'upgrade' }
    if (!(Test-GesherStoreLicenseIdentity -License $match -Catalog $catalog)) {
        throw 'The expected SKU and offer token did not match'
    }
    foreach ($license in @(
        @{ SkuStoreId = 'OTHER/0010'; InAppOfferToken = 'upgrade' },
        @{ SkuStoreId = 'SKU/0010'; InAppOfferToken = 'other' },
        @{ SkuStoreId = 'sku/0010'; InAppOfferToken = 'upgrade' }
    )) {
        if (Test-GesherStoreLicenseIdentity -License $license -Catalog $catalog) {
            throw 'A different add-on identity matched the catalog'
        }
    }

    $product = @{
        ProductKind = 'Durable'; StoreId = 'SKU'; InAppOfferToken = 'upgrade'
        Skus = @(
            @{ StoreId = 'SKU/0001'; IsTrial = $false; IsSubscription = $false },
            @{ StoreId = 'SKU/0010'; IsTrial = $false; IsSubscription = $false }
        )
    }
    if ((Find-GesherStorePurchaseSku -Product $product -Catalog $catalog).StoreId -ne 'SKU/0010') {
        throw 'Checkout must use the configured SKU even when another SKU is listed first'
    }
    $product.InAppOfferToken = 'other'
    if (Find-GesherStorePurchaseSku -Product $product -Catalog $catalog) {
        throw 'Checkout accepted a product with a different offer token'
    }
    $product.InAppOfferToken = 'upgrade'
    $product.Skus[1].IsTrial = $true
    if (Find-GesherStorePurchaseSku -Product $product -Catalog $catalog) {
        throw 'Checkout accepted a trial'
    }
    $product.Skus[1].IsTrial = $false
    $product.Skus[1].IsSubscription = $true
    if (Find-GesherStorePurchaseSku -Product $product -Catalog $catalog) {
        throw 'Checkout accepted recurring billing'
    }
    $product.Skus[1].IsSubscription = $false
    $product.Skus += $product.Skus[1]
    if (Find-GesherStorePurchaseSku -Product $product -Catalog $catalog) {
        throw 'Checkout accepted an ambiguous SKU'
    }

    foreach ($storeStatus in 'Succeeded', 'AlreadyPurchased') {
        $result = Resolve-GesherStorePurchaseStatus -Status $storeStatus -TimeoutSeconds 1
        if ($result.Status -ne 'PendingVerification' -or $result.Entitlement.Status -ne 'Unavailable') {
            throw "A Store outcome alone cannot verify a purchase: $storeStatus"
        }
    }
    foreach ($storeStatus in 'NotPurchased', 'NetworkError', 'ServerError') {
        if ((Resolve-GesherStorePurchaseStatus -Status $storeStatus -TimeoutSeconds 1).Status -ne $storeStatus) {
            throw "The purchase outcome was lost: $storeStatus"
        }
    }
}

'Store entitlement provider checks passed'
