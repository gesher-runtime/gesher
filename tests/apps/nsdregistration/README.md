# Windows NSD registration gate

This APK is an ordinary installed application. The Windows services track runs
it on an isolated persistent Gesher device, through the production installer,
activity launcher, assigned UID and `ProcessRecord`. Registration calls use
`Context.getSystemService(NsdManager.class)` and the public executor overload.
The fixture does not construct a service, replace a provider, inject an owner,
set a permission/native capability flag, or register from a standalone VM.

The gate is maintained in:

- `tests/windows/run-nsd-registration-contract.ps1`
- `tests/nsd/registration_peer.py`
- `tests/dex-services/gesher/services/NsdRegistrationOwnerDump.java`

## Profiles

| Scenario | Value preparation | Required observations |
| --- | --- | --- |
| `public` | SDK36 public String attributes, including UTF-8, embedded NUL, null and empty | Real probes, unsolicited announcement, independent TXT decode, TCP connection, QU and small legacy answers, unregister and retirement |
| `reuse` | Binary seed resolution plus the pinned hidden hostname preparation below | Two services sharing the actual default host; same listener immediately reused after unregister; new port/epoch; no old goodbye after the replacement is observed; withdrawing one leaves the other queryable/connectable |
| `death` | Binary profile | Kill only the observed application PID after checking its creation time and actual command line; the same persistent server emits the three PTR goodbyes and retires its resources |
| `uninstall` | Binary profile | Actual installer removal, package absence, process exit, independent goodbyes and native retirement |
| `conflict` | Binary profile | An independently owned valid SRV answer conflicts with the proposed name; the application receives a renamed instance; that instance probes, advertises, accepts TCP and retires |

Each scenario runs on actual IPv4 and IPv6 addresses of the selected adapter.
Missing IPv6 is a failed prerequisite, not an inferred success. A fresh random
service type is used per scenario. Only that service, its separately owned seed
type and host records derived from those values are queried or retained.

### Exact binary preparation and its limits

SDK36 exposes `NsdServiceInfo.setAttribute(String, String)`. Its byte-array
overload is hidden. The full binary scenario first resolves `Seed-1` or `Seed-2`
from the independent peer using the already implemented public resolution API.
The returned value carries null, empty and opaque byte values unchanged. The
fixture verifies those values and their total size; it does not mutate an
undocumented live array or manufacture a Parcelable stream.

A resolved value also owns the seed's hostname. SDK36 exposes `getHostname()`
but no public setter. Therefore the binary preparation explicitly invokes the
genuine pinned `setHostname(String)` method with null through reflection,
clears addresses using public `setHostAddresses`, and verifies the hostname is
null, the address list empty and the actual selected `Network` unchanged.
This **one additional hidden value operation** is part of the pinned source
profile; the binary preparation is not described as entirely public SDK36
behavior. All register/unregister calls remain public. The `public` scenario
starts from a fresh value and performs no hidden value operation.

The replacement seed and TCP listener are prepared before unregister. The next
registration request is submitted immediately after the terminal unregister
callback, with the same listener identity. A canonical attribute digest is
checked again immediately before each registration and compared independently
against the peer's decoded wire values. Seed-resolution failure is reported as
a prerequisite failure and supplies no registration evidence.

The binary set occupies exactly **1300 attribute-policy bytes** and **1299 TXT
RDATA bytes**; the null flag omits the equals byte. The public set occupies 28
policy bytes and 27 RDATA bytes. Raw first-write datagrams, their full lengths
and SHA-256 hashes are preserved independently of the canonical attribute hash.
TXT decoding compares null versus empty and every opaque byte, including NUL,
0xff, 0xfe and non-UTF-8 sequences.

## Persistent owner and transport observations

The runner starts the real `com.gesher.app.Launcher` in a new `ANDROID_DATA`
directory. It installs this APK, then invokes the existing shell
`RuntimeCommands am start` to start the real application process. The fixture
finds its `Network` through `ConnectivityManager` and the actual `LinkProperties`
containing the peer's address. The manifest requests INTERNET and
ACCESS_NETWORK_STATE because this fixture enumerates networks; it does not
claim that registration itself requires the latter permission.

The standalone owner-dump client loads the genuine `android_runtime` library
and uses the same host-shell control identity as `RuntimeCommands`. It makes
only a read-only `Binder.dump` request to the actual remote registration owner.
The returned PID must match the persistent server. Native counters must show
real sockets, both watchers, a completed poll and a completed send while active;
after retirement all native counters and client/request counts must be zero.
The runner also checks Windows' actual UDP endpoints for that server PID.
Unavailable counters or an OS query error never become zero substitutes.

The dump and runner require `pending_withdrawals` explicitly. This counter tracks
service-owned IDs awaiting backend withdrawal admission after a cleanup rejection;
it is distinct from the backend's subsequent goodbye and socket retirement.
Active readiness and final retirement both require it to be zero. An old dump
schema that omits the field is rejected, and an empty client/request map cannot
hide a still-pending cleanup ID.

The independent Python peer uses Windows `WSARecvMsg`, including actual
destination, arrival-interface and hop metadata. QU delivery is proven by the
unicast destination, not inferred from the reply contents. A separate ephemeral
source port exercises conventional legacy replies: ID/question echo, no
cache-flush, TTL at most ten seconds, and a 512-byte limit. Large TXT is omitted
with TC; small public TXT is returned completely. Independent packet decoding
handles legitimate compression and checks that multi-record datagrams fit the
actual family MTU. It never imports the production DNS writer/reader.

Relevant primary descriptions are Microsoft's
[WSARecvMsg documentation](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nc-mswsock-lpfn_wsarecvmsg),
[WSAMSG structure](https://learn.microsoft.com/en-us/windows/win32/api/ws2def/ns-ws2def-wsamsg),
and [IPv4 socket options](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ip-socket-options).
The selected real Windows headers are separately used to compile layout,
constant and function-signature assertions for x64 and ARM64.

## Build and run

Use the normal test APK builder; its output is `nsdregistration.apk`:

```sh
bash tests/apps/nsdregistration/build.sh out/app \
  --framework-res out/framework-res/framework-res.apk \
  --framework-classes out/framework/classes --core-classes out/coremod
```

The APK also compiles against the official SDK36 `android.jar`. SDK stubs are
compile inputs only and are not packaged in the APK. Stage the APK beside the
other APKs in the framework distribution. The normal services-test DEX build
automatically includes `NsdRegistrationOwnerDump`.

The existing services track dot-sources the registration runner. It preserves
the old browse/resolve and other service checks. The runner requires 64-bit
Windows, Python, actual IPv4/IPv6 multicast and the built APK/services DEX. In a
prepared services-track environment an explicitly selected actual adapter can
be used with:

```powershell
. tests/windows/run-nsd-registration-contract.ps1 -NsdRegistrationInterfaceIndex 7
```

That number selects an existing adapter; it does not create or modify one.
The default chooses a connected preferred IPv4 adapter and its actual IPv6
address. The runner restores its environment, uninstalls the owned package,
shuts down its isolated device and closes the independent peer in `finally`.
Each process-control deadline is a fixture bound, not a production NSD timeout.

Receipts remain under the generated `nsd-registration-*` directory: app values,
peer raw packets, source endpoints and interface metadata, owner dumps, actual
UDP counts, launcher/install logs and a consolidated gate receipt with APK/DEX
hashes. No canceled workflow is restarted or dispatched by these files.

## Qualification boundary

The authored gate has been compiled and its PowerShell parsed locally; it has
not been executed on Windows. Compilation and valid Python fixture checks do
not prove Windows sockets, ART, Binder, packet delivery or application lifecycle.

Even a successful ordinary-adapter run qualifies only the scenarios above.
Actual interface loss/return at the same index and low-MTU single-record
fragmentation remain required separate owned-interface merge gates. The runner
does not alter host interfaces. It records whether an oversized single-record
datagram was actually observed, and sets `fullWindowsMergeQualification=false`
until the separate evidence is supplied. Subscription APIs and simultaneous
probe tie-breaking beyond the pinned source behavior are outside this gate.
