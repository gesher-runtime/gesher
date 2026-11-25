# WebStorage: remaining full-storage-key contract

The real Windows qualification at
[run 45498551083](https://github.com/gesher-runtime/gesher/actions/runs/45498551083)
passed on WebView2 131.0.2903.86 and 157.0.4341.46. Both runtimes discovered
persisted IndexedDB data before revisiting an origin, enumerated five actual
storage keys, deleted two observed keys and preserved the other three payloads.
This establishes the inventory and exact-key deletion primitives. It does not
establish effective quotas for arbitrary partitioned keys.

## Enumeration needs a separate quota primitive

The pinned [Android bridge][aw] obtains all full `StorageKey` values, then calls
`QuotaManager::GetUsageAndQuota` separately for every key. Its successful rows
contain the embedded origin and that key's usage and quota. Projecting several
partitions into one origin must preserve this provider behavior; adding all
partition usages and attaching a first-party quota is not its operation.

[CDP `Storage.getUsageAndQuota`][cdp-implementation] accepts an origin and
unconditionally creates a **first-party** storage key. Its return value is not
a query for an arbitrary observed full key. The current public
[Storage protocol][cdp] still exposes no storage-key parameter on this method.
The browser's [quota-internals interface][quota-internals] provides bucket
inventory and usage, but no effective per-key quota.

Normal same-origin partitions commonly have equal quotas. That observation is
insufficient to make first-party values interchangeable:

- A DevTools quota override belongs to an exact storage key. A first-party
  override does not determine the quota of an existing third-party key with
  the same embedded origin.
- Android's `GetUsageAndQuota` returns zero usage and `kNoLimit` for a key with
  unlimited storage policy. The DevTools variant instead gathers usage and can
  constrain its reported quota using available capacity. The underlying
  [quota manager][quota-manager] explicitly implements these different paths.
- A bucket's `StorageBucketInfo.quota` is its configured bucket metadata. It is
  not the current effective quota of the containing storage key.
- `navigator.storage.estimate()` and legacy
  `navigator.webkitTemporaryStorage.queryUsageAndQuota()` both use
  [`GetUsageAndReportedQuotaWithBreakdown`][quota-host]. On newer runtimes that
  API can deliberately report a static quota; it is not the Android bridge's
  effective-quota query. Microsoft's [static quota policy][static-quota]
  documents the distinction independently. Legacy `requestQuota()` uses that
  same query and only limits its callback value to the requested amount; it
  does not provide a separate effective-quota operation.

Therefore full `getOrigins` remains unimplemented until the backend can obtain
the effective usage/quota result for each full key, including override and
unlimited-policy behavior, or a complete alternative provider supplies this
contract. Registry success does not justify a fabricated zero, a visited-origin
registry, a copied live database, or an assumed quota formula.

## Origin deletion is independently implementable

The same [Android bridge][aw] deletes the file-system and IndexedDB data types
through [`StoragePartitionImpl::ClearDataForOrigin`][partition]. That method uses
a trusted-origin filter. It deliberately includes third-party data embedded
under the requested origin's schemeful site while preserving the requested
origin when embedded under a different site.

The [storage-key implementation][storage-key] makes the rule explicit:

- A first-party key matches the complete origin, including scheme and port.
- A third-party key matches its top-level schemeful site.
- The serialized `^0` form contains that site explicitly; `^31` and nonce
  forms have an implicit site derived from their embedded origin.
- An opaque top-level site cannot equal a nonopaque HTTP(S) origin's site.

A production bridge can combine the already qualified inventory and exact-key
deletion with a runtime-derived schemeful-site resolver. It must preserve
observed full keys, use the actual application profile for inventory/deletion,
avoid navigating application WebViews, retain asynchronous callback/controller
ownership, and report failures instead of silently completing partial work.
An exclusively owned InPrivate controller can obtain the runtime's actual
frame storage key from intercepted local documents without visiting an
application origin in its ordinary profile. [WebView2 profile isolation][profiles]
allows an ordinary and off-the-record controller with the same profile name;
the resolver must verify that distinction and close the controller.

The implementation and its real Windows fixture must qualify this resolver,
including same-site subdomains, port and scheme differences, implicit-site
keys, and preservation of data types outside Android's deletion mask. A public
suffix approximation or treating the fixture's known origins as a production
registry does not meet this requirement.

### Production implementation plan

The public entry remains `Host::ClearOriginData(origin, callback_id)`. Admission
continues through `PostGlobal`; one queued operation owns a `GlobalWork` lease
until its terminal callback. Operations run serially on the existing WebView
STA, with a bounded deadline and no nested message pump.

Each operation creates an exclusively owned ordinary controller in the same
environment/default profile as application WebViews. It verifies the actual
profile, navigates only to the IndexedDB/quota internal documents, and reads a
stable, paginated snapshot of the browser's complete key inventory. A separate
off-the-record controller with the same profile name resolves sites. Before its
first navigation, it installs an all-resource local responder, disables cache
and service-worker use, and puts the network offline. It verifies its InPrivate
profile and never reads or writes application data. The browser supplies both
top-frame and child-frame storage keys, including out-of-process iframe IDs.

Selection completes before deletion starts. The bridge retains each original
observed key and uses the existing exact-key CDP method with precisely
`file_systems,indexeddb`. Unknown serialization, malformed inventory, incomplete
site resolution or a failed prerequisite ends the operation with an error.
There is no partial-success callback. If a later individual deletion fails,
already completed deletions cannot be rolled back; the overall callback still
reports failure. Event subscriptions, timers and owned controllers are removed
on the STA. Late controller-creation callbacks retain their admission lease
long enough to close any returned controller before permitting STA teardown.

The independent production fixture calls the public Host method against the
same seeded application profile. Raw CDP is reserved for setup and readback,
not for the operation being tested. `getOrigins` and worker capability flags
remain unchanged.

### Local validation of the implementation

The native host compiled with warnings treated as errors for x64 and ARM64,
both static and DLL loader definitions. The complete existing standalone
probe linked for both x64 loaders. The shared registry expressions are
byte-for-byte the previously qualified expressions. The selector/parser test
passed 71 checks under AddressSanitizer and UndefinedBehaviorSanitizer,
including same-site versus exact-origin matching, nonce and ancestor-chain
keys, opaque sites with and without a precursor, and rejection of incomplete
site resolution before publishing a deletion set.

These local results do not qualify the new private-controller production path.
The separate actual-Host Windows fixture is required before claiming that path
as runtime-verified; the earlier raw-CDP qualification is not its substitute.

[aw]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/android_webview/browser/aw_quota_manager_bridge.cc
[cdp-implementation]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/content/browser/devtools/protocol/storage_handler.cc
[cdp]: https://chromedevtools.github.io/devtools-protocol/tot/Storage/#method-getUsageAndQuota
[quota-internals]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/storage/browser/quota/quota_internals.mojom
[quota-manager]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/storage/browser/quota/quota_manager_impl.cc
[quota-host]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/content/browser/quota/quota_manager_host.cc
[static-quota]: https://learn.microsoft.com/en-us/deployedge/microsoft-edge-browser-policies/staticstoragequotaenabled
[partition]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/content/browser/storage_partition_impl.cc
[storage-key]: https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/third_party/blink/common/storage_key/storage_key.cc
[profiles]: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/multi-profile-support
