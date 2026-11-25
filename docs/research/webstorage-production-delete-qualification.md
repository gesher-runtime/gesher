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

## First actual Windows attempt

[Run 45590458033](https://github.com/gesher-runtime/gesher/actions/runs/45590458033)
tested `c17855100f529b6cb3faaa6e8e4ec3a5a52e4ca5`, tree
`e7dec0b88476f4c482ae643ed893960be8bfd775`. Its native build succeeded; both
Windows jobs stopped at the fixture's profile-identity preflight before any
production deletion call. WebView2 131.0.2903.86 and 157.0.4341.46 successfully
returned the actual default profile's empty name, which the fixture incorrectly
rejected. The candidate backend contained the same nonempty-name assumption.

Both conditions are now corrected to retain the observed empty name. Successful
property reads, the actual ordinary/private mode, nonempty profile path and
exact resolver profile-name/path comparison remain required. No directory name
is substituted for the profile name. This agrees with the
[SDK's documented empty default profile name](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2environment10?view=webview2-1.0.4258.31#createcorewebview2controlleroptions).
That failed attempt provides no production-deletion acceptance; a new exact-head
run must exercise the operation after this correction.

The corrected profile candidate `346ce277d21bed3956925dd6f94d4a04b37a22b4`
ran in [45591406389](https://github.com/gesher-runtime/gesher/actions/runs/45591406389).
Both runtimes passed the profile checks, five persisted-key measurements and
the expanded thirteen-key fixture. They then exceeded the CDP deadline while
writing the nested A → B → A case, still before any production deletion call.
The next candidate gave outer A a distinct owned document URL, as described
below. Both runtimes still stalled at that nested seed in run 45593812298; the
URL distinction did not establish a fix.

## Actual Host admission and native crash at 107d7fa5

[Run 45597266059](https://github.com/gesher-runtime/gesher/actions/runs/45597266059)
tested `107d7fa57c3daf66511d475ceeb09ec907c969cb`, tree
`f9266df78377922a5c3f3ef37988362c03c9c13c`. The build produced artifact
`15013338305`; its upload log records ZIP SHA-256
`6754c9b5eef048583bee699a5051605e8a5aadccd71f0d1270f29cee4430ce19`.
Attempt 1 stopped at the existing first cold controller-setup gate, before the
storage test. Failed jobs were rerun at the unchanged head and artifact.

In attempt 2, jobs `139149103447` (Windows 2022, runtime 131.0.2903.86) and
`139149104127` (Windows 2025, runtime 157.0.4341.46) successfully completed the
real nested write and discovered all fourteen positive full keys. The
document-source-aware filter change therefore crossed the earlier fixture
blocker. Both then started the actual Host deletion and crashed with native
access violation `0xC0000005` (process exit `-1073741819`). Runtime 157 recorded
two ordered, exactly-once successful callbacks on the owner STA before the crash;
runtime 131 did not reach the callback summary.

The [exact attempt-2 receipt](webview-runtime-qualification-107d7fa5-attempt2.json)
retains both complete-log hashes, source/artifact identities and actual anchored
observations. No final preservation, network-isolation, shutdown or `QUALIFIED` result was
produced. Callback success alone is not acceptance. The fault location is under
investigation; no specific lifetime defect has been established. The standalone
probe now records an unhandled native exception's code, thread, access address,
fault module/RVA and bounded stack from a copied exception context. A separate
pre-COM subprocess deliberately raises `0xe0470001`; the Windows wrapper must
observe that exact nonzero exit and an actual probe frame before trusting the
diagnostic. The real storage operation still runs in its own process with the
original checks. No production behavior or acceptance rule has been relaxed.

The first diagnostic build at `4178e10814949f5f9a13fd709c91473ffb9de835`
passed in [run 45600590577](https://github.com/gesher-runtime/gesher/actions/runs/45600590577).
Both Windows jobs then stopped in the preceding PowerShell worker log-fixture
step: its 44 scenarios completed, but the simulated exit status from the final
negative case escaped into the outer command invocation. Consequently neither
the native fault control nor the actual storage/worker qualification step ran.
The fixture runner must restore its caller's native exit-status state while
preserving assertion failures; this failed run supplies no new native evidence.

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

For the nested case, outer A uses the separately supplied document
`/storage-nested-top.html`, while middle B and inner A use `/storage.html`.
Both paths are exact owned fixture responses with the same page code and real
origin. This URL distinction is not an established fix: both Windows runtimes
still stalled at the nested seed in
[run 45593812298](https://github.com/gesher-runtime/gesher/actions/runs/45593812298),
before any production Host admission. A separate Linux Chrome `151.0.7922.34`
execution of the exact embedded scripts and native-style awaited CDP expression
completed with both outer paths, a real 262,144-byte write, closed database and
an observed `https://storage-a-nested.invalid/^31` key. That is browser fixture
evidence, not WebView2 production acceptance. The Windows gate still requires
the real nested write and its distinct positive registry entry.

The fixture replaces its legacy resource filters with
[`ICoreWebView2_22` document-source filters](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_22?view=webview2-1.0.4258.31)
before navigating. Microsoft documents that the
[legacy filter](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.4258.31#addwebresourcerequestedfilter)
omits requests initiated by cross-origin iframes; the B-initiated inner-A load
must receive the owned response too. Positive network controls remove and
restore the same document-source filter. Bounded trace records preserve real
message hops, resource callbacks and inner-document state; a timeout additionally
records the actual frame tree. None of these diagnostics supplies a storage
result, key or successful completion.

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

Cleanup checks each server's health after its final socket drain, and records
the sentinel's accepted connections, request count, received bytes and
unattributed bytes. An unclassified nonempty request cannot become a successful
storage qualification merely because it was discovered during `Stop`.

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
