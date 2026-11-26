# Gesher Free and paid editions

Implementation plan for review, with product decisions, code ownership and release criteria.

## Product decisions

- Gesher has a permanent Free edition and a one-time upgrade sold through Microsoft Store. A redeemed Store promotional code grants the same paid entitlement.
- Publish through Microsoft Store and continue distributing the regular Windows installer. End portable distribution. Keep the same capability policy in both supported channels.
- Free runs third-party APKs within the capability profile below. Both editions use the same ART runtime and allow bundled native libraries on supported ABIs inside per-app isolation. Paid adds specific capabilities of the runtime and its services.
- Production isolation is required for third-party APKs in both editions. Paid isolation settings operate above that minimum.
- Preserve and migrate existing data. Existing users can receive individual Store codes at the maintainer's discretion. Prior installation alone confers no paid entitlement.
- Keep the current microG and Aurora support at launch in Paid, across the Store and regular installer channels. Qualify distribution and existing workflows on both channels before release.

## Capability policy

| Capability | Free | Paid |
| --- | --- | --- |
| APK install and run | Supported APKs using the Free profile, subject to admission and runtime checks | Same entry points with the full implemented profile |
| ART, packaged DEX and native libraries | Standard execution, JNI and packaged libraries on supported ABIs, inside the sandbox | Same baseline and explicitly classified advanced runtime capabilities |
| UI, input and graphics | Normal windows, keyboard and pointer input, standard graphics and app lifecycle | Additional classified desktop and graphics features |
| Media | Standard audio and video playback | Capture and advanced media integrations where supported, with normal consent |
| Storage and app interactions | Private Android storage, Android-internal clipboard and ordinary intents and scoped grants between eligible Android apps | Opt-in host file access, Windows clipboard and sharing, and advanced storage settings |
| Network | Outbound public Internet and WebView, subject to Android permission. Block LAN, host and loopback access and inbound listening | Additional local-network functions as implemented and expressly enabled |
| Background | Work associated with ordinary foreground lifecycle and required cleanup | Persistent jobs, services, restart and other independently classified background behavior |
| Devices and notifications | Host camera, microphone, location, notification delivery and other host device integrations are restricted | Available with applicable Android and Windows permissions and hardware support |
| Special access and settings | Ordinary runtime permissions for Free capabilities. Basic isolation, security updates, diagnostics, uninstall and data migration | Individually classified special access and advanced isolation and integration controls |
| Image selection in place of capture | Restricted | The existing approved, explicit setting remains off by default and retains the existing scope and behavior |
| microG and Aurora | Installation, activation and use of the integrations are restricted | Preserve existing installation, updates, accounts and supported workflows, subject to the common isolation requirements and Store certification |

Planning defaults are paid Android notification posting as well as paid Windows delivery, and Free standard ABI translation and ordinary APK splits needed to run otherwise eligible apps. Classify advanced engine and packaging capabilities individually through observable operations. Initial review candidates include running an independent native executable, debugger integration and advanced installation workflows. Confirm the support status and contract of each candidate before offering it as a paid capability.

## Contribution policy

Apply the [edition contribution policy](../CONTRIBUTING.md#free-and-paid-editions). As the gates are implemented, record one versioned capability catalog in the repository and use it for the implementation, documentation and tests.

1. Classify each new app-visible capability before merge. The paid edition is the default for newly supported capabilities. Free expansion requires a maintainer decision recorded in the catalog. Assign capability identifiers to observable operations, with the relevant Android contracts, entry points and enforcement points.
2. Fixes to an existing contract inherit the classification of that contract. Security, consent, lifecycle correctness, data integrity and isolation required by Free features are available in both editions. Document changes that materially expand an existing capability.
3. An unavailable or unimplemented capability is unavailable in both editions. Offer an upgrade only when the installed build can actually provide the requested operation with an active paid entitlement, subject to independent requirements. Maintain truthful feature discovery and Android error and callback behavior. Continue applying the project scope rules to each edition.
4. Each change that crosses an edition boundary must show the app-visible behavior in both editions, the host enforcement and the relevant production-path test. Paid configuration must preserve the common isolation minimum. Make a catalog change and obtain maintainer approval before moving an existing Free capability to Paid.

## Architecture and enforcement

| Component | Responsibility |
| --- | --- |
| Store provider and Control Center | Authenticate entitlements through Windows, run purchase and redemption UI, restore purchase, display localized price and license state, notify the trusted Gesher server of changes |
| `SystemServer` and `GesherService` | Own the edition decision and capability policy for the running profile. Consume verified entitlement state. Resolve UI reasons from trusted decisions |
| `PackageInstallerService` and `PackageManagerService` | Evaluate verified package artifacts and known requirements before committing an install or update when applicable. Publish feature availability. Keep updates atomic. Carry policy through existing installed packages and profile changes |
| `ActivityTaskManagerService`, `ActivityManagerService`, native process bootstrap | Check launch, secondary components and process creation, including existing-process delivery, jobs, notifications, services, providers, receivers, child processes and restarts. Check the effective caller and target as required by each operation |
| `AppSandbox`, native spawn and Binder | Apply the per-package token, verified caller identity, approved handles, file ACLs and network policy before app code runs. Contain native children and other processes that consume untrusted APK content as required by the approved threat model |
| Device services, AppOps, storage and Windows bridges | Check the capability at the actual grant, dispatch or resource-open boundary. Preserve Android and Windows consent. Give WebView, helper processes and indirect callers the same effective restrictions |
| Windows launcher, CLI and Control Center | Display typed outcomes using shared upgrade wording. Windows entry points include APK association, shortcuts, existing app actions and notification activations. CLI returns a structured failure and can run headless |

**Admission and runtime.** Evaluate reliable, mandatory requirements before launch. A required capability known to be paid can block with an upgrade explanation before starting the app. An optional feature or declared permission alone does not establish a mandatory requirement. Package name and a static scan are insufficient to enforce behavior. Evaluate unknown or conditional requirements when the app actually performs the operation. Map each blocked call to the appropriate Android result and expose a separate, authenticated reason to Gesher's Windows UI. Keep Android's ability to handle a refusal normally. Deny direct access at the Windows boundary too, including when a raw native operation has no Android call from which to derive an upgrade prompt.

**Upgrade UI.** For an explicit user action or identifiable foreground request, show the capability and package, with Upgrade, Restore and dismiss actions. Coalesce repeat denials per app and capability. Background attempts return the expected Android result and update the Control Center without opening a Store window. Gesher owns the wording. Distinguish tier denial, checking, unavailable licensing, ordinary permission denial and absent hardware or implementation. Resume a blocked user action only with the applicable confirmation. A runtime request that already returned an error can be retried by the app, or by relaunch after the user chooses to do so.

**Network.** Start with a Windows-enforced per-package outbound Internet policy. Omit private network capability and loopback exemptions in Free. Verify effective restrictions separately for IPv4 and IPv6, DNS and redirects, Windows network profiles, proxies or VPNs, and WebView2 traffic. Use the same boundary for any host-side fetcher acting for an app. AppContainer network capability alone needs validation against the exact LAN and host restriction. Release requires supported enforcement on each advertised Windows version without granting a broader capability as fallback.

**Isolation.** Use ADR 0090 and issue #314 as the starting point. Before shipping external APKs, require production evidence for per-package isolation, trusted Binder identity, token and child handling, private data, and the brokers used by both editions. A launch that cannot establish the required isolation fails with an actionable technical reason. The installation's ordinary runtime and the experimental development switch documented at the planning baseline are insufficient production evidence. Separate development fixtures from all signed public builds.

**microG and Aurora.** Inventory the existing setup, Control Center, CLI, APK and split installation, upgrade, app launch and background paths. Require Paid for the integrations at install and activation boundaries, including use by already installed clients. Preserve current signer verification, privileged permission checks, consent, accounts and app data. Reevaluate activation after restore and stop paid integration components on verified loss of entitlement. Test that Free apps cannot obtain paid functions indirectly through an installed service. Use [installer behavior](installer.md) and [microG contracts](microg.md) to define exact parity checks. Confirm the permitted way to offer both integrations in the Store listing and submit them for certification. A change required by certification comes back to the maintainer as a product decision.

## Commerce and distribution

Track user acceptance of the commercial EULA and the published Free and Paid offers in the channel design. The [commercial license](../LICENSE) defines the acceptance requirement and what the entitlement identifies. Use the same edition classification in both offers, and validate the acceptance and disclosure flows before release.

1. Publish a Free MSIX identity and a `Durable` add-on with `Forever` lifetime. Configure one canonical Store product. In the desktop app, purchase on the UI thread, attach the owning HWND and use a non-elevated process. Refresh with `GetAppLicenseAsync` after purchase, on restore, on startup and on Store license change. Verify the Gesher package family and the exact SKU Store ID and offer token in the valid add-on map. Deduplicate transactions. Treat cancellation and a transaction awaiting confirmation as distinct states. See the [installer entitlement runbook](verification/store-installer-entitlement.md).
2. Test the regular installer first on a fresh Windows account and machine against that same Store add-on. Test an officially supported signed package identity with external location if appropriate. Verify commerce eligibility separately from identity registration. Include installing the Store app after the installer, installing the installer after the Store app, updates and side-by-side identity or file-association behavior. If direct verification fails, record the platform evidence and return to the maintainer to choose among documented alternatives. Candidates include a Store-installed licensing component and a separate verification service. Accept paid entitlement only from the approved verified provider.
3. Use Partner Center single-use promo codes for the paid add-on. Redeem with Microsoft, then use the same restore path. Provide a redemption link and support instructions without collecting codes in Gesher. Microsoft documents a quota of 1,600 redemptions per six-month period for non-game apps and add-ons. Use Store sale pricing for supported discounts and test the checkout on target Windows versions. Confirm separately with Microsoft before offering single-recipient percentage or fixed-amount discount codes.
4. Honor an active license returned from the Microsoft Store cache offline, with no Gesher-imposed periodic online check. Use Free for first use with no verified entitlement, while preserving all app data. A confirmed inactive entitlement blocks new paid operations, revokes broker access and gracefully stops affected processes. Close processes as needed to remove access retained by already open handles. Show a temporary Store error as an availability issue with retry and restore actions. Use Paid while Store supplies an active license. Retain Free access and app data when license status cannot be verified. Apply the normal Android and Windows consent checks in both editions.
5. Maintain Store updates for the MSIX and the regular updater for the installer. Produce both from the same versioned capability policy and equivalent production gates. Remove new portable ZIP and portable Setup paths from public releases and documentation after migration checks pass. Allow existing regular and portable profiles to migrate explicitly. Stop the old runtime, validate the source profile and schema, migrate atomically with rollback, retain app data and settings, and validate shortcuts and package associations. Serialize access to any common profile. Keep save data accessible for export or migration even when its app is blocked by edition.

Microsoft references: [Store licensing](https://learn.microsoft.com/en-us/uwp/api/windows.services.store.storecontext.getapplicenseasync), [durable add-ons](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/add-on/create-app-submission), [promo codes](https://learn.microsoft.com/en-us/windows/apps/publish/generate-promotional-codes), [sale pricing](https://learn.microsoft.com/en-us/windows/apps/publish/put-apps-and-add-ons-on-sale), [external-location package identity](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps), [Store policies](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies) and [network capabilities](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations).

## Work order and exit criteria

| Stage | Acceptance evidence |
| --- | --- |
| 1. Commerce and packaging prototype | Exercise purchase, cancel, redemption, relaunch, offline, account change and update in a real Partner Center test package on Windows. Try direct restore of that entitlement in the regular installer. Confirm Store treatment of APK loading, microG and Aurora. Record blockers for maintainer decision |
| 2. Compatibility baseline and isolation | Choose representative APKs from the existing compatibility evidence and maintainer scenarios. Record workflow, APIs and expected results. Run using packaged production entry points with per-package containment and policy enforcement, including native libraries and real WebView2 network traffic |
| 3. Capability catalog and checks | Assign each launch feature and existing relevant integration to the catalog. Prove post-install checks for launch, updates, runtime APIs, background delivery and direct Windows bridges. Reject unclassified new capabilities through review and catalog validation |
| 4. Purchase UX, migration and channel parity | Verify localization, accessibility, repeat handling, CLI exit behavior, retained settings, downgrade and restoration, legacy and portable import, installer update and cleanup. Check both supported release outputs |
| 5. Store certification and rollout | Use a limited test release. Verify real purchase plus promo redemption, capability enforcement, regression evidence and data preservation. Publish both channels after Store eligibility, license restore in both channels, production isolation and paid microG/Aurora parity are demonstrated |

The current work starts at the probes in stages 1 and 2. As of 2026-11-26, the
maintainer confirmed there is no Partner Center product and upgrade add-on
configured for the cross-channel purchase test. An earlier partial free
release predates these offers. Reach a real restore verdict after setting up
the product, add-on, signed test packages and a Windows account with a test
purchase or redeemed code.

Coverage should include separate APKs for Free-only workflows, a known paid mandatory requirement, an optional paid capability, native code and WebView. Include camera permission denial even with paid entitlement, missing camera hardware, notification and background behavior, already running app processes, upgrade in the middle of an operation, and attempts to resolve restricted resources through other app or host components. Host fixtures can exercise classification and state transitions. Only packaged Windows runs can establish actual Store commerce and OS-level isolation. Use the existing compatibility inventory to select workflows and API families, then qualify the selected workflows on the Free profile.

### Follow-on implementation

- The Store entitlement provider and diagnostic share the live Windows Store
  query. The release provider uses a packaged catalog and reports unavailable
  until a real catalog is assigned. Windows PowerShell checks cover the
  unconfigured provider, package-family mismatch and unrelated base licenses.
  The checkout preparation selects the exact one-time durable SKU. Checkout
  requires the owning desktop UI thread, and a Store purchase result is
  followed by a fresh entitlement query. The real checkout, package identity
  and installer restore still require the Store test product.
- A server-local Free trial can restrict the Windows notification delivery
  capability in the AppContainer internet-only development profile. Ordinary
  Android permission, posting, active notification and cancellation behavior
  remains in that trial. The installed launcher strips the trial selector.
  The Windows ART publication fixture exercises the production service with
  explicit Windows toast and image test boundaries.
- The Free trial's Android location broker returns real providers as
  unavailable, keeps Android permission checks and authorized mock
  behavior, and skips host and IP-based position acquisition. The
  independent WebView2 geolocation permission path needs its own enforcement
  and validation before claiming complete host-location restriction.
- Production activation still requires verified Store entitlement,
  authenticated tier delivery to the running server, and production
  containment. Scope a downgrade to new launches, already active brokers and
  persisted Windows notification activations. Complete the remaining app
  capability checks and upgrade presentation before publishing either offer.

### Representative app workflows

The maintainer named these examples. Pin the tested APK build and signing identity when obtaining the package. The following are proposed acceptance scenarios to qualify with that build. Apply the same capability policy to all apps.

| App | Free acceptance scenario | Paid comparison |
| --- | --- | --- |
| Waze | Check launch and the app's actual behavior when location is unavailable. Where the app permits it, open the map and search for a destination with outbound Internet. Trigger the Gesher upgrade reason on an actual blocked location request | With supported real location and user consent, compare location-dependent behavior. Preserve ordinary hardware or permission failures |
| Mercantile | Check launch and ordinary flows away from camera capture. Use only a designated test account or supported test mode for sign-in and other account flows. Trigger the Gesher upgrade reason at a capture request if reachable | Compare real camera capture under supported conditions and the already approved image-selection setting, without submitting a banking transaction |
| Wikipedia | Open, search for and read articles online. Verify that ordinary navigation stays within the Free profile | Repeat the same baseline with Paid |
| Pardes | Confirm the intended app identity and test opening, browsing or searching, and reading its content. Record its actual storage and network requirements | Repeat the same baseline with Paid |

Record differences caused by app behavior, unavailable existing Gesher support, device availability or tier policy. For Waze and Mercantile, qualify usable workflows within the restricted permissions and record cases where the app itself declines to continue. In parallel, use deterministic test APKs for camera and location refusal and upgrade UX.
