# Verify Store entitlement in the regular Gesher installer

Status on 2026-11-26: the diagnostic runs against the Windows license API. Cross-channel restoration of a Gesher purchase still needs a Store-associated Gesher app, its durable add-on and an entitled test account.

## What Windows provides

`StoreContext.GetAppLicenseAsync()` queries the current app and user. It uses Windows' cached licenses offline. `StoreAppLicense.AddOnLicenses` contains currently valid durable licenses, keyed by the add-on **SKU** Store ID. Check the package family and the configured entry's SKU and `InAppOfferToken`. `StoreAppLicense.IsActive` describes the base app. Microsoft documents `StoreLicense.IsActive` as reserved and currently always true. [License query](https://learn.microsoft.com/en-us/uwp/api/windows.services.store.storecontext.getapplicenseasync) · [Add-on license map](https://learn.microsoft.com/en-us/uwp/api/windows.services.store.storeapplicense.addonlicenses) · [StoreLicense](https://learn.microsoft.com/en-us/uwp/api/windows.services.store.storelicense)

The existing Inno Setup product installs ordinary desktop binaries. Its `AppId` GUID is an uninstaller identifier. Windows supports assigning package identity to a Win32 installer via a signed MSIX with external location and matching application manifest. Microsoft's Store testing instructions describe installing a published Store version once before running a locally built package associated with that app. Test the separate external-location scenario with the actual Gesher entitlement before committing to it. [Current installer](../../tools/package/gesher.iss) · [External-location package identity](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps) · [Store commerce testing](https://learn.microsoft.com/en-us/windows/uwp/monetize/in-app-purchases-and-trials#test-your-in-app-purchase-or-trial-implementation)

## Diagnostic

Use `tools/store/Test-StoreEntitlement.ps1` with Windows PowerShell 5.1. It reads package identity and calls the Windows license API. It prints a single JSON result with no account identifiers, product identifiers, receipts or raw license data. `-Inspect` reports whether the process has package identity and whether `GetAppLicenseAsync()` returns an active base license. It has no interpretation as paid access. For a matching-identity test, supply the exact package family, add-on SKU Store ID and Partner Center add-on product ID exposed to the API as `InAppOfferToken`.

```powershell
powershell.exe -NoProfile -File tools/store/Test-StoreEntitlement.ps1 -Inspect

powershell.exe -NoProfile -File tools/store/Test-StoreEntitlement.ps1 `
    -ExpectedPackageFamily $gesherPackageFamily `
    -ExpectedAddOnSku $gesherUpgradeSkuStoreId `
    -ExpectedOfferToken $gesherUpgradeProductId
```

Run the verification from a process with the distribution's effective package identity, for example from its test launcher's PowerShell child if Windows gives that child the same identity. The probe verifies that prerequisite explicitly. An ordinary terminal can have a different identity from the launcher. If the distribution's launch context does not carry through to PowerShell, run the same query from a diagnostic build of the launcher itself. Keep the probe outside public artifacts and any runtime authorization decision.

| Exit code | Outcome | Meaning |
| --- | --- | --- |
| 0 | `InspectionOnly` | The Windows call completed in the current process context |
| 0 | `ExpectedAddOnPresent` | The supplied package identity, SKU and offer token match a current Windows license entry |
| 2 | `PackageIdentityMismatch` | Process identity is absent or differs from the expected package family |
| 2 | `ExpectedAddOnAbsent` | Current license result had no matching valid add-on entry |
| 3 | `AddOnIdentityMismatch` | The selected SKU returned a license with a different SKU or offer token |
| 3 | `Unavailable` | The probe could not determine the result. Inspect `Stage` and `HResult` |

## Store and installer experiment

The diagnostic calls the same license query as the in-process
`Get-GesherStoreEntitlement` provider in
`tools/store/GesherStoreEntitlement.psm1`. The provider's release catalog is
unconfigured until Partner Center assigns a package family, add-on SKU Store
ID and offer token. A configured provider returns `Active` on an exact
currently valid match, `Inactive` on a completed lookup without a match, and
`Unavailable` when the catalog, process identity or lookup is unavailable.
Each call asks Windows for the current state, including its own offline
cache. Diagnostic overrides only affect that diagnostic call.

The provider is currently a first implementation stage. Future packaged
callers must query on a background worker in the correct Windows user and
package context, handle Store license-change invalidation, and authenticate
the result at the server boundary. The PowerShell diagnostic's JSON and exit
status remain observations for the experiment.

Restore calls `Get-GesherStoreEntitlement` on the worker to query Windows
again. To prepare purchase testing, also supply the add-on's `StoreProduct`
Store ID. `Get-GesherStorePurchaseOffer` looks up the associated durable
product in the background and requires the exact product Store ID, offer
token and a unique matching SKU without trial or recurring billing.
Verify the configured lifetime is `Forever` in Partner Center before
publishing. `Start-GesherStorePurchase` runs
on the owning non-elevated STA window thread. It associates that HWND with
the offer's `StoreContext` and starts checkout for the verified `StoreSku`.
The caller can poll the returned operation on the dispatcher
without blocking it. `Complete-GesherStorePurchase` runs on a worker. A
successful or already-owned Store outcome triggers a new entitlement query.
Only an active matching license yields `Verified`. If Store reports a
purchase but the license has yet to be confirmed, the worker returns
`PendingVerification`. [Store SKU purchase](https://learn.microsoft.com/en-us/uwp/api/windows.services.store.storesku) · [Desktop window ownership](https://learn.microsoft.com/en-us/windows/apps/develop/ui/display-ui-objects)

1. Obtain the Gesher MSIX package family, exact add-on SKU and offer token from Partner Center for the same Free application and `Durable` add-on. Publish to an appropriate test audience and acquire the add-on in one test Store account. Keep a separate account without the add-on for controls.
2. On a fresh Windows test profile, run the Store build first. Record `ExpectedAddOnAbsent` before acquisition, `ExpectedAddOnPresent` after acquisition and `ExpectedAddOnPresent` when offline while Windows returns an active cached entitlement. Repeat acquisition on a clean account using a single-use Store promo code. Take note of the Windows build and package version for each observation.
3. Install the ordinary Gesher installer using its supported path, and run the probe in its effective launch context. Compare the stock installer with the permitted, signed package identity with external location if one can be associated with the Store product. Test installation with the Store build present and installer-first followed by Store acquisition on a freshly prepared profile. Test Store build removal on a disposable profile to establish whether installer restoration depends on keeping the Store package installed. Confirm both installers can be updated and that runtime identity and protocol associations behave as intended.
4. Repeat on the control account and after changing Store accounts. Record the exact identity or Store error category when expected results differ. Independently confirm what release and signing arrangement Microsoft supports, then require positive restoration in the installer to claim direct cross-channel restore.

Do not use the probe to infer a permanent storefront limitation from an unsupported test host or an unrelated package. If the real installer experiment cannot restore the add-on, report which stage blocked and what Microsoft documents for that arrangement before choosing an alternative.

## Evidence obtained locally

The Windows PowerShell 5.1 session inherited the Codex package identity. No Gesher package was found in the current user's package registrations. Three checks ran against the real API on this machine:

| Check | Observed |
| --- | --- |
| Inspection in the inherited identity | `InspectionOnly`, `HasPackageIdentity=true`, `BaseLicenseActive=true`, exit 0 |
| Deliberately different package family | `PackageIdentityMismatch`, no license read, exit 2 |
| Matching current process family with a deliberately absent SKU | `ExpectedAddOnAbsent`, `BaseLicenseActive=true`, `HasExpectedAddOn=false`, exit 2 |

The local checks validate diagnostic execution and rejection of an unrelated active base license. There is no observed Gesher purchase, positive add-on lookup, offline cache test or direct cross-channel restore result yet.

Windows PowerShell 5.1 also compiled the desktop bridge and attached a real
`StoreContext` to a hidden WPF owner HWND on the calling STA. It rejected an
empty HWND. The separate provider test checks the unconfigured purchase guard
and the requirement to re-query entitlement after successful Store statuses.
No purchase UI or transaction was started during these checks.
