# PR 644 desktop AppContainer qualification

## Local Windows results

These checks exercised the desktop candidate with `GESHER_SANDBOX=appcontainer-trusted`
and disposable profiles. The shipping default and the user's installed copy were
unchanged. The tested runtime used the current native Windows binaries, rebuilt
ARM64 translator and app JAR, and newly generated boot images with matching core
and framework JARs.

| Route | Observed result |
| --- | --- |
| Installed APK | Launcher and guest loaded all five boot-image spaces. The current lifecycle APK reached `onResume`, looked up the USB service, verified `/data` capacity and device-state isolation, and used the real NUL device. Its native child used its Android UID and the admitted Binder connection. |
| Scoped and single-process launches | The packaged `--system-server` and `--single-process` commands loaded the five boot-image spaces and ran the lifecycle activity to `onResume`. Both exited with status 0. |
| Standalone DEX | The guest had a low-integrity AppContainer token and loaded the three core boot-image spaces. The probe verified argv, stdin, read-only input, independent file positions, denial of an adjacent file, restoration of the input ACL and exit status 23. |
| Per-app isolation override | The signed UI fixture displayed a real window. The Windows token was queried on its retained process handle. The native warning named the app and the full Windows-account access, with Cancel as the default. Cancel kept the original process alive and isolated. Approval and restoration each retired the original process and the next launch used the requested token. |
| Portable launcher and migration | Cancel in the native installation dialog returned 1223 and installed nothing. Install then Open ran the protected app. Opening an already installed version showed no installation dialog. A generated portable archive worked from a path with spaces. Moving a populated copy preserved the APK and app data and left the original profile unchanged. The candidate also refused to attach to a live unisolated device and preserved the existing APK and app data after a normal shutdown and isolated restart. |

The portable archive reused fixed suppliers from an existing build. The clean
Inno installation, Windows registration and upgrade, and the full CI shard
matrix still require a successful run.

## Roborock startup

The supplied Roborock 4.76.08 APK (`com.roborock.smart`, version code 100944)
was copied to the workspace, installed in a new profile and launched through the
installed desktop command.
The kernel reported a low-integrity AppContainer token for the process that
owned the app's window. `SplashActivity` returned a successful start result
and presented its first 400×800 frame about five seconds after invocation.
The app proceeded to `RNAccountActivity` and started its React Native bridge.

The app's installed `libpag.so` lists `libmediandk.so` as a dependency and
imports MediaCodec entry points including `AMediaCodec_createDecoderByType`.
The run translated the real ARM64 library and reached the activity without an
unresolved MediaCodec symbol or `UnsatisfiedLinkError`. This establishes the
startup path. Media playback, account sign-in, device pairing and device control
were outside this check.

On closing the window, Roborock's `Application.onTerminate` called
`Context.stopService`. Gesher had already marked the process as exiting,
and the service caller lookup rejected its application thread. The app returned
exit status 1 during cleanup. The relevant `maybeExit`, `processByPid`,
`sender` and `stopService` method bodies match main at `e2685dc71`.
This comparison is at source level. The test device shut down and removed its
device state.

The APK and its test copy had SHA-256
`f26660665dcb39b4688e976d16ff2dc270f31867780c3b1628f0263a6fa03203`.
The test used the rebuilt candidate based on integration commit `850e69880`.
Its `dalvikvm.exe` had SHA-256
`d345ee97149869368621cbf596d3205b485a454f89605ca65aed58d72fd4028b`.
The run logs and runtime hashes are under the local
`out/qa314/roborock-startup-25652/` evidence directory.

## Native and desktop follow-up checks, 2026-11-27

These are local execution results for the candidate above `3c7fe0dd2`, with the
explicit AppContainer setting and disposable profiles. They do not qualify the
shipping default, the installer or the hosted matrix.

| Check | Observed result and limit |
| --- | --- |
| Native2 directory operations | The exact x86 and ARM64 APKs downloaded from run `45946701754` reached `native2: all passed`, `onResume: all passed` and process exit 0. The held directory survives rename and replacement of its old name. The narrow relative-operation path leaves creation, complex paths, symbolic-link following and brokered storage on their established resolvers. |
| Directory negative controls | An independently rebuilt runtime with the original Bionic adapter reached the actual APK's `remove_all` assertion and reproduced `EBADF`/`SIGABRT`. A separate live guest with an advanced directory descriptor reproduced the wrong seek cookie before the fix. Independent installed probes then verified the correct first entry and advancing cookie with isolation both on and off; all 61 physical/virtual Bionic assertions passed. A failure to start an app was not counted as a negative control. |
| RTL and Windows fonts | The Windows font lookup used `/mnt/c/WINDOWS/Fonts` in a separate-volume run with the installed Arabic AppContainer app. The client-paint and window-DC captures each matched the 8,000 expected pixels per color region. Keyboard and injected Win32 mouse messages produced the expected selections. Full-content composed capture mirrored those regions in the RTL cases. Desktop screen capture was unavailable in the disconnected local session, leaving the orientation of the visibly composed client and caption unresolved. The test continues to require composed capture. |
| Media | The protected video app exited 0. Its real PCM capture contained 880.0 Hz and 660.1 Hz tones, each lasting 2.005 seconds. The capture was written inside its own CE directory and copied to the host artifact directory before uninstalling the fixture. |
| TSF | The protected app loaded the real test service from the read/execute runtime tree and completed composition and reconversion, including restricted-editor rejection. The local session had no foreground window, so only the local harness omitted that precondition. The tracked CI test retains its foreground requirement. |
| Environment diagnostics | The existing native host regression passed with the rebuilt diagnostic JNI library. A missing child retained its failing result and recorded the actual child exit status. The diagnostic does not change fixture return codes, dump the environment or establish why the hosted APK failed. |

Native source, runtime and APK hashes are retained in
`out/qa314/native-ci-repair/final-inputs.json`; that snapshot includes Bionic
SHA-256 `4b2a88eef3812a62d1b3722dcf9f1813fd9969992b82252ade062b49cb4d553f`.
The corresponding guest and host logs are in that directory. Desktop proofs are
under `out/qa314/matrix-local/`. Symlink creation was unavailable to the contained
guest on this machine; the existing resolver for that operation was preserved.

The private USB runtime USER32 initialization failure, installed CardWake and
archive RPC, clean installation/upgrade, full hosted shards, final default
launch routes and real Windows Hello consent/cancellation remain merge gates.

## Additional repairs after CI run 45966509354

These are local, separately bounded execution results. They do not establish
an integrated shipping-default or installer qualification.

| Check | Observed result and limit |
| --- | --- |
| Inherited working directory and Exec3 | The installed protected fixture failed its first native spawn when only the launcher's working directory was outside the readable runtime; changing only that directory made the old runtime pass. The repaired ordinary spawn inherits the existing process directory handle. Explicit shell `cd` and fork-broker routes retain the caller's actual directory. The tracked installed-device segment then passed from the original host directory with isolation both enabled and disabled. A real Android ELF/fork witness read the expected relative marker and preserved the parent's directory. |
| Protected fixture authority | Actual admitted Shell operator writes are visible to Java, ELF and native app readers. The app's attempted property writes, UID environment override and foreign force-stop are denied. These controls exercise the production policy; they do not grant test packages additional authority. |
| Directory API consistency | The unchanged public directory and Scandir contract includes were compiled as actual Android x86 and ARM64 libraries. Matched original executables each reproduced the three inconsistent-field failures. Rebuilt executables each completed with zero failures. `opendir` now adopts the same descriptor enumeration as `fdopendir` and `scandirat`, keeping actual inode/cookie values consistent. The obsolete arithmetic guard on an opaque `INT64_MAX` cookie was removed. Linux host fault/maximum-cookie controls still require CI execution. |
| Binding attachment fixture | Actual ART execution reproduced the missing attachment stamp. Supplying the fixture's exact installed APK object as its process code generation made all six cleanup/attachment scenarios pass. Production generation checks are unchanged. The local metadata APK was cached separately, rather than the exact CI APK. |
| Camera and media | A protected app completed legacy preview-to-JPEG reopen, Camera2 preview/still capture and the built-in capture Activity's thumbnail and provider-written 640x480 JPEG. The explicit host Media Foundation fixture was used; this PC reports no physical camera. No-permission, guest-only fixture selection, foreground revocation and provider write failure controls retained their failures. Recording passed all 17 checks, with 35 completed captures and one buffer-lost callback; independent decode contained more than 30 ordered frames. Both actual muxer APK ABIs passed independent container, metadata and Ogg verification. |
| Camera lifecycle | A stale configured session is closed instead of starting preview after cancellation, pause or permission loss. Minimizing the owned camera window caused an actual Android pause, cancellation and routing of result zero to the original caller. Its callback while minimized was not observed. The exact normal capture APK still delivered both successful callbacks. The extracted production permission/lifecycle fixture passed 73 host assertions; those are distinct from the actual Android runs. |
| Hello assertions | Each unavailable/cancelled callback is checked against its own observed focus state. This does not test an enrolled Windows Hello approval or cancellation. |

The directory before/after logs and source/executable hashes are in
`out/qa314/native-ci-repair/directory-apis/`. The installed Exec3 logs and 13
source/runtime/artifact hashes are in `out/qa314/native-ci-repair/cwd-final/`.
ART binding logs and hashes are in `out/qa314/system-binding-repair/`; camera,
recording, muxer and interruption evidence is in `out/qa314/media-second/`.

Both E2E shards, both windowing shards and both compilation shards passed in
run `45966509354`. Its unchanged Java preflight stopped before running tests
because Maven returned HTTP 429 while fetching SQLite JDBC. This was a download
failure, not evidence about the unexecuted Java tests. The full matrix remains
required after these source changes, and all earlier outstanding merge gates
remain in force.

## Held-directory Inotify follow-up

The local successful-write route now verifies full file identities for the
writing object, the reopened single child and the retained watched directory.
Its coarse identity index selects candidates only. When full identity is
unavailable, it does not synthesize a directory event from pathname equality.
Ordinary Windows notification close tracking remains available through a
separate query-only generation handle and an exact-object `ReOpenFile` probe;
it does not duplicate the writer's write-access file object.

The final diagnostic-instrumented Windows projection ran both unchanged Native2
APKs from the cached CI artifact. Both reached `native2 files: ok` and
`native2: all passed`. A separate held-directory fixture closed its original
writer before renaming the directory, retained the watch, rejected a write to
the replacement at the old name, and observed modification and close through
the moved directory. Its rename, no-wrong-event, close and cleanup fields all
passed. This does not establish renaming a directory while its writer is open.
The four source/fixture hashes, both APKs, Bionic input and runtime identity are
recorded in `out/qa314/native-ci-repair/inotify-diagnostic/final-v2-inputs.json`.

The whole-source host fixture adds deterministic lookup/rename/replacement,
coarse identity collision, unavailable full identity and close-generation
controls. Its Linux execution and sanitizer checks still require CI. The
smaller record-state fixture retains all ten original assertions and passes
when compiled on Windows; its declared query boundary does not prove actual
handle or sharing semantics. Physical ReFS and new performance measurements
are outside these local checks. Historical timings are unchanged.

## Camera provenance and contract-fixture follow-up

The camera pipe now carries the backend selected by successful initialization.
An older untagged producer is reported as unknown, and the explicit software
fixture does not claim Media Foundation execution. The unchanged track-58
assertions pass for the actual protected IMFSourceReader path and reject only
the MF proof for software and older-producer controls. Actual malformed and
fixture-mismatched tags are rejected before frame requests; the earlier client
still captures preview and JPEG with the updated producer. Source, runtime,
APK and control identities are in
`out/qa314/camera-provenance/final-inputs.json`. This remains an isolated test
picture, not physical camera qualification.

The GMS binding fixture now supplies its exact installed code generation, and
the external-storage fixture checks the complete policy including that
installation's actual owner. Original Windows ART runs reproduce each failure;
the corrected fixtures pass 35 and 54 checks respectively.

Installer Shell fixtures now use the real live device's authenticated operator
admission. A genuinely enrolled probe tests foreign installer ownership while
retaining harmless own-session creation and abandonment. Naked forged-UID
transport denial and the original legacy service-denial tests are separate
checks. The full protected installer run passes with both the signed and stock
unsigned test APKs; an independent PID/creation-time/token witness confirms the
unsigned probe's low-integrity AppContainer. Exact source, APK, DEX, native,
runtime and log identities are in
`out/qa314/contracts-repair/installer-inputs.json`.

The unmodified SAF fixture also passes 1,161 actual native-peer checks locally.
That does not resolve its hosted callback failure. WebView networking, the
isolated Windows 2022 native task-menu failure, private USER32 and the earlier
installer/default/interactive gates still require qualification. No criterion
was weakened to count these local results as a green integrated matrix.

## Cross-volume document selection follow-up

The hosted SAF cancellation was reproduced with an existing document on D:
and the process working directory on C:. The original guard checked real
ancestors correctly, then treated the synthetic `/mnt` namespace prefix as a
filesystem ancestor. Its failed identity query canceled the ordinary selection.
The repaired guard shares the provider's existing drive/UNC-root predicate,
checks the real volume root and stops before synthetic parents. Identity errors
on real objects still fail closed; Android data, obb and sandbox descendants
remain blocked before URI grants or document creation.

Matched original and repaired ART runs reproduced the failed callback and
successful real document URI respectively. The repaired focused run passed 84
checks. The complete updated native Binder corpus passed 1,216 checks with each
of the real C: and D: working directories, including three actual persistence
restarts per run. Protected descendants, aliases, parent traversal and absent
objects retained cancellation without a grant. Source, JAR/DEX, runtime and log
identities are in `out/qa314/saf-hosted/final-inputs.json`. Temporary SUBST
experiments were removed and are not the before/after proof. Actual UNC-share
execution remains unavailable and the hosted rerun remains required.

## Protected graphics fixture follow-up

The nested service-first graphics run reproduced `0xc0000142` before Android
attachment: its disposable private desktop lacked the runtime capability grant
used by ordinary launchers. The fixture now adds only the five production
desktop rights to its newly generated GUID desktop. It changes no existing
desktop, window station or integrity label. Protected control commands use the
real verified operator, retaining the separate legacy path.

The original three modes pass in both protected and legacy executions. Native
frame ownership, service/PID/job lifetime, hidden unapproved frames, background
visibility and explicit-headless assertions remain intact. The configured
CMake executable and all 83 final receipt artifacts were independently verified
in `out/qa314/window-promotion-qa/guid-inputs.json`. This tests same-user
AppContainer graphics, not the separate private SYSTEM runtime. The shared
runtime capability is not an assertion of foreign-app isolation on that fixture
desktop; the next hosted matrix remains required.

## Inotify legacy-query fallback

The first complete Linux host-model run in `45995014400` passed its original
ten record checks and 62 Linux reference checks, but failed one of 306
production-model checks: denial of the legacy coarse identity query also
prevented directory metadata validation. The repair treats that query as an
index optimization. A separate attributes query may establish directory and
no-reparse metadata while the full writer, child and held-parent identities
remain mandatory. Failure of both metadata queries remains fail-closed.

The original routing assertion is unchanged. New current-source controls
separately deny parent and child attributes; they do not impose new semantics
on historical source controls. A strict MSVC Windows probe reproduces the old
child-open failure and passes with the repaired header, verifying the same
actual full file identity, denied metadata, traversal/directory/stream selectors
and exact cleanup. Sources, original header, binaries and logs are in
`out/qa314/inotify-attributes/final-inputs.json`. The full Linux model and
sanitizer rerun remain pending. Simultaneous DOS-name and coarse-identity
failure, physical ReFS and a real reparse-file probe are outside this evidence.

## RTL composed and visible evidence

An actual active-desktop AppContainer window displayed all four expected color
regions correctly, at 8,000 of 8,000 pixels each. The same retained window's
composed `PrintWindow` readback was reflected. The shared graphics fixture now
maps those composed rows to physical screen coordinates only when the actual
production `GesherRtlFrame` window property equals one. It retains
`PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT)`, with no CPU-buffer fallback
or changed pixel expectations. The optional hosted visible diagnostic checks
the live process creation time, AppContainer token, foreground window, input
desktop and bounded unobscured region; it records counts without saving screen
pixels.

The exact original three RTL cases pass with the corrected composed helper,
including all six Hebrew cursor movements. A real APK with incorrectly mirrored
forced-LTR content fails all three cases on the two incorrect row halves. The
final receipt, `out/qa314/visible-rtl/final-composed-inputs.json`, binds 432
independently verified artifacts and the earlier actual-display receipts.

The final patched active-display repetition was unavailable after the session
became inactive. The earlier visible run used an older portable runtime whose
four relevant production and fixture sources match the reviewed sources; it
does not qualify the entire current runtime. The new visible diagnostic and
composed correction do not replace the original RTL assertions or establish a
green hosted matrix. Separate private-runtime, interactive and default-enable
qualification remains open.

## Numeric loopback TCP relay qualification

The contained runtime now retains a host-side TCP endpoint for numeric IPv4,
IPv6 and mapped loopback destinations; it transfers bounded bytes and typed
options over authenticated Binder record pipes. It does not hand a host socket
to the app. Ordinary external sockets retain the application's actual Windows
network identity. Socket creation checks the real AppContainer capabilities,
including rejection when a no-INTERNET APK spoofs the old environment flag.

The source-generation/PID-birth admission, bounded stream credits, module and
fork lifetime, same-application listener controls, and actual native option
results were reviewed independently. Two subsequent QA findings were repaired:
final close explicitly retires inbound credit when the peer writes before
reading, and AF_UNSPEC replays acknowledged native option values before
publishing a disconnected descriptor. A replay error keeps the original relay
attached and propagates the actual failure. An isolated Windows probe then
injects WSAENOTSOCK on the second native replay: the exact extracted production
functions retain the relay, its acknowledged options and actual bidirectional
traffic, then a retry restores native options and detaches it. Its immutable
receipt is `out/qa314/network-replay-failure-probe/qualified/inventory.json`.
This fault control uses a fixture no-op for poller wakeups and is not a complete
ART/admission fault injection.

The immutable TCP-v2 inventory contains 53 artifacts, including all 18
capture-time source snapshots and actual Android application logs. The final
commit removes one extra trailing empty line from each of three new files;
`out/qa314/tcp-final-style/changes.json` binds the exact before/after hashes.
Their earlier immutable evidence was retained, not repinned. Its native fixture passes
nine groups and 27 framing controls. A peer first writes 1 MiB, then receives
all bytes accepted from the app plus EOF; an omission control times out. A
separate five-second receiver stall preserves accepted data, while restoring
the old 1.5-second truncation semantics fails. These are explicit semantic
negative controls, not full historical-source execution claims.

After including main through 7beab9a3a, the actual runtime and protected Binder
admission targets rebuilt successfully. Admission and 1,340 broker lifetime
checks pass with zero retained allocations; both relay fixtures pass. Fresh
installed APK and DEX executions verify Socket/NIO, refusal, cancellation,
typed options and two AF_UNSPEC reconnects, including a native option change
while disconnected. The no-INTERNET APK still receives EACCES. Their 37 copied
artifacts and exact new runtime identities are preserved separately in
`out/qa314/network-main-sync-qa/qualified/inventory.json`; the earlier immutable
TCP-v2 evidence was not overwritten.

This qualifies the numeric loopback TCP slice for draft CI. It does not qualify
explicit source/interface binding, UDP, WebView, positive SO_LINGER, the private
service route or the shipping default. Unsupported operations retain explicit
failure instead of fabricated success. The historical source transition adds
only the three now-modified socket inputs to the original 48, each bound to its
actual immutable Git predecessor; all earlier predecessors, archived
measurements and rejection controls remain in force.

## Owned desktop mask matrix follow-up

Hosted run 46006971740 on both Windows versions passed setup and cleanup of
all original owned-object controls. The same private and lowbox subjects fail
USER32 on WinSta0/Default but load it successfully on the new full-grant station
and desktop. This localizes the difference to object/context policy; it does
not establish which access bit or original-object property is responsible.

Seven additional diagnostic cases now isolate a fresh GUID desktop under the
unchanged current station (old mask, READ_CONTROL added, full mask), and fresh
station/desktop pairs with the original effective masks and READ_CONTROL added
to either or both. Each case reads back exact effective per-token rights and
low integrity on owned objects. Existing WinSta0/Default security is only read;
it is never granted, restored or switched. The original four rows and all real
launch gates remain intact. Separate fixed reports do not displace the bounded
original USER32/GDI trace.

Strict MSVC compilation and ordinary desktop mechanics, non-SYSTEM rejection,
and missing-image cleanup controls pass. The 18-artifact receipt is
`out/qa314/owned-desktop-matrix/final-inputs.json`; actual private/lowbox matrix
execution remains pending hosted CI. Ordinary mechanics do not qualify that
matrix, the service UI, physical USB or default enablement.
