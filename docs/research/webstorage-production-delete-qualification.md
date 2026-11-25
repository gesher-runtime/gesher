# Production WebStorage deletion qualification

`webview2_probe.exe --storage-delete` calls the public production
`gesher::webview::Host::ClearOriginData` method. The standalone probe links
`compat/webview/webview_host.cc` directly. Setup and observation use the real
WebView2/IndexedDB fixtures and quota registry; there is no fixture CDP deletion
call, replacement Host, supplied origin registry or manufactured browser result.

This gate accompanies the partition-aware backend. It is expected to fail on
the earlier first-party-only implementation. A compiled fixture is not evidence
that a runtime passed it: retain the exact source commit, artifact digest,
runtime version, loader and complete Windows output when recording results.

## State and operations

The fixture first writes and closes all five baseline databases: A first-party,
C under A, B first-party, C under B and A under B. It observes the writer browser
process exit, reopens the same actual ordinary profile, initializes the browser's
persisted IndexedDB inventory and verifies all five positive quota measurements
**before revisiting an application origin**.

It then adds same-site subdomain, nondefault-port, HTTP/HTTPS and nested-ancestor
cases. All fourteen keys are measured before another complete browser exit.
The production Host receives only the configured profile path and the requested
origin. No application Host exists, and the fixture supplies no controller,
frame ID, initialized storage registry or list of keys to delete.
An unrelated ordinary-profile network controller starts the fresh shared browser
after the second seed-process exit; it neither revisits a seeded storage origin
nor calls IndexedDB or registry APIs.

| Data context | After deleting A | After deleting HTTPS loopback | After deleting HTTP loopback |
| --- | --- | --- | --- |
| A first-party | Absent | Absent | Absent |
| C under A | Absent | Absent | Absent |
| B first-party | Preserved | Preserved | Preserved |
| C under B | Preserved | Preserved | Preserved |
| A under B | Preserved | Preserved | Preserved |
| Subdomain of A, first-party | Preserved | Preserved | Preserved |
| D under that subdomain | Absent | Absent | Absent |
| A on port 8443, first-party | Preserved | Preserved | Preserved |
| E under A on port 8443 | Absent | Absent | Absent |
| HTTP loopback, first-party | Preserved | Preserved and read back | Absent |
| C under HTTP loopback | Preserved | Preserved and read back | Absent |
| HTTPS loopback on the same host/port, first-party | Preserved | Absent | Absent |
| D under HTTPS loopback | Preserved | Absent | Absent |
| A inside B inside A (`^31`) | Absent | Absent | Absent |

The first two calls enqueue the same A deletion before waiting. Both must report
one ordered successful callback on the Host owner thread. Repeated deletion
must leave the same expected state. A further request uses the production
resolver's first fixed sentinel as a legitimate requested origin. Its existing
partitioned inventory forces the real same-site sentinel/retry branch; the
fourteen-key state must remain unchanged. The HTTPS and HTTP requests are separate:
the HTTP pair is read in full after HTTPS deletion, before HTTP deletion starts.
The final HTTP deletion is admitted immediately before `Host::ShutdownIdle`;
shutdown must drain it, deliver its callback, close its controllers and join.
The pending callback is checked immediately before admission closes. Bounded
one-second shutdown retries pump the fixture STA between waits, keeping its
ordinary observer controllers responsive while the independent Host STA drains.
New work after shutdown must be rejected.
Each callback measures `CoGetApartmentType` and must report a consistent
non-caller STA, rather than merely assuming the Host's apartment from its source.

The ordinary result observer checks quota measurements before reopening the
databases. Every removed database must then be genuinely absent; the probe
aborts its creation transaction instead of creating an empty replacement.
Every preserved database must contain its complete label-specific 262,144-byte
payload. The observer's browser process must exit after closure.

## Keys and data outside the deletion mask

For ordinary and cross-site frames, keys come from the browser's real
`Storage.getStorageKeyForFrame` response, using the actual iframe DOM owner for
out-of-process frames. The nested A → B → A case is not assigned a fabricated
frame ID. Its write must add exactly one distinct, positive registry key with
the browser's `^31` serialization; the original observed string is retained.
Full inventory validation rejects collapsed, missing or unexpected keys.

Every one of the fourteen contexts has a localStorage sentinel. Each of the six
first-party contexts also has a persistent host-only cookie. Final database
readback/absence checks require these sentinels to remain. Cookie names are
distinct across the same host's port and scheme cases, because cookies are not
partitioned by port. Cookies are not claimed to have been tested in third-party
frames.

A and B also write real 8,192-byte files through the origin-private file-system
API. Every writable stream is closed, and every byte is read back. A's file must
be absent after its origin is deleted; B's entire file must survive. The gate
requires this actual API and never supplies a substitute file-system result.
CacheStorage preservation is not exercised by this fixture: its retained quota
usage would require a distinct inventory accounting case. The mask remains the
production `file_systems,indexeddb` mask.

## Resolver isolation checks

The fixture verifies that its actual application profile is ordinary, records
its profile name/path, and checks that reopened controllers use the same path.
It instantiates the shared production `EnvironmentOptions` and
`HostBrowserArguments` helpers, so concurrently opened environments use the
same options as the actual Host. The result observer and ordinary network
control must also report the same browser process ID.
It uses the same loopback TCP server as the worker diagnostics. An initial
browser navigation intentionally reaches that server and verifies both its
response and an independent request/connection counter. This positive control
ends before the storage seed browser exits.

The subsequent production resolver receives HTTP and HTTPS origins using that
listening port. The accepted TCP connection counter must not change throughout
production operations and storage observation. Counting accepts also detects an
accidental TLS connection, which an HTTP-path counter alone would miss. This
establishes absence of origin network connections for the controlled cases;
it does not purport to measure unrelated browser background networking.
After the production Host has joined, a second real ordinary-profile network
control must succeed. This checks that resolver network settings did not leave
the application profile offline.

A second independent TCP server supplies the unrelated ordinary-profile control
document. That document performs sequential no-store fetches every 25 milliseconds
from before production admission through the final callback. The gate requires
zero fetch/body failures and actual successful completions during each deletion
interval (A, sentinel, HTTPS and HTTP), using browser completion timestamps and native
admission/callback timestamps from the same machine's UTC clock. After the loop
stops, its successful-fetch count must exactly equal the independent server's
control-path request count. This catches an offline setting that affects an
ordinary application controller during deletion, even if networking recovers
when the resolver closes. Control traffic and resolver-escape traffic use
different listening ports and counters.

The production implementation must separately verify the resolver controller's
actual InPrivate flag and profile name before origin navigation, install its
owned all-resource responses before navigation, disable service workers/cache,
and establish its offline network condition before proceeding. Unexpected
resources need owned failure responses. A sentinel that is same-site with the
requested origin must not be accepted as evidence of a third-party key; the
resolver needs a distinct sentinel and must validate the observed key and
current document/iframe identities. An interface or internal-page failure is a
failed operation, never a successful partial deletion.

## Running and acceptance

Build both existing standalone probe variants with the pinned SDK and normal
production source. The fixture depends on the shared loopback server's
`Connections()` accessor added in worker diagnostic commit
`8c5fcc19820db86d4df8908f88d68eb55081d070`. Run the static-loader executable in a
separate process; its internal budget is 660 seconds, including cleanup. The
Windows wrapper `tests/windows/webview/StorageDelete.ps1` uses the existing
`RunTimed` helper with a 690-second process limit and is included by track 37.
The fast WebView workflow can dot-source that same wrapper after loading its
existing `RunTimed` helper and setting `$Dir` to the downloaded artifact.

The wrapper requires every expected inventory, payload/absence, preservation,
callback, shutdown and network marker, a single final qualification marker and
exit zero. It rejects every `FAIL` and `UNAVAILABLE` outcome. The separate quota
registry diagnostic retains its existing diagnostic-only unavailable behavior.

Actual runtime qualification is still required. This fixture does not establish
effective per-key quotas or full `getOrigins`, nonce-key lifetime semantics,
every Windows policy combination, or service-worker interception support.
