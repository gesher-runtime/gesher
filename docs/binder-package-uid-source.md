# Package/install UID authority prerequisite: unqualified source

Disposition: SOURCE UNQUALIFIED, P1 TRANSPORT UID AUTHORITY STILL OPEN. This is
concrete source progress beyond the PID foundation, not the requested complete
launch registry/bootstrap. Do not merge into qualified primary or reuse old AOT
acceptance. A fresh exact-source peer, rebuilt component and new mandatory full
stage remain required. No security conclusion follows from a clean checkpoint.

Exclusive tree: `C:/work/gesher-binder-peer-identity-current`, branch
`wip/binder-peer-identity-current`. Starting clean docs HEAD:
`637ac74d84bdfeccdfe83ef6a346d09cc9ec5207`, previous transport source:
`8404eb005d8a4e0e9b9e023dc3fc86bc58116639`, qualified primary base:
`caeee6a84882b327c86fdc05590a6ace98d27133`. This follow-up source:
`69739417566b932cf0cd580fb6bb6c76ddeda1e3`.

| Path | Source blob |
| --- | --- |
| app/java/com/gesher/server/ActivityTaskManagerService.java | dc7f9170416ed2bfb7c6a11b4ab7ab4456dee90c |
| app/java/com/gesher/server/ApkPackage.java | e1dd1faa8a707f166c4cb0198ef60ad9dbd0276f |
| app/java/com/gesher/server/Installer.java | 96c7f45ab58ab30fbec24603e4eaf2084746c496 |
| app/java/com/gesher/server/PackageIdentity.java | 94331afba69c4ac59b21bdf65492c1156baba72d |
| app/java/com/gesher/server/PackageManagerService.java | 0c75a7e69d1b4c01d91ae5e83c4d2314f25de577 |
| app/java/com/gesher/server/SystemServer.java | 88ceb9538e2b258dfc29f0ec85fe4629a986fc47 |
| compat/framework/services_zygote.cc | 513c279b2f13be347a7b685bc310960d92eb4960 |
| compat/framework/services_zygote.h | c3139f6d1c6d20a3288f2def6195411912438d40 |
| tests/dex-package-identity/com/gesher/server/PackageIdentityFixture.java | 8a2a70f24d52fdb8c2aa7738e1af3522cb47fb48 |

## Implemented package/install state

PackageIdentity is an immutable server-side package name, application appId and
random UUID install incarnation. The project pins AOSP `android-16.0.0_r4` in
`aosp.lock`; normal appIds use that framework's Process.FIRST_APPLICATION_UID
through LAST_APPLICATION_UID (10000..19999). Allocation chooses the first free
slot. This is a bounded single-user allocator, not a claim of complete AOSP
Settings/AppIdSettingMap allocation-policy equivalence. UID is not PID. Update
and unchanged install preserve UID/incarnation; uninstall removes the mapping;
reinstall allocates a fresh incarnation even when a freed UID is reused.

Installer persists UID/incarnation in six-column packages.list records. Old
four-column records migrate once, reserving existing identities first and writing
the new registry before exposing migrated packages. Malformed records, duplicate
package/UID assignments and failed atomic replace abort instead of dropping an
identity or deleting the old registry. Install/uninstall restore the in-memory
entry if registry replacement fails. A registered APK that cannot parse now
aborts boot instead of silently freeing its reserved UID. This availability
change and atomic-move provider behavior need runtime review. No power-loss
durability or filesystem isolation claim is made.

PM assigns command-line APK identities only for that server session (not persistent
install records); installed packages reload their recorded identities. SharedUserId
manifests are rejected. Explicit PM userId requests other than 0 throw; multiuser
and shared UID support remain open. ApplicationInfo for PM packages, getPackageUid,
getNameForUid and getPackagesForUid return exact assigned mappings; unknown UIDs
return null and cannot select a first/all-package fallback. Standalone parsed APKs
outside PM retain historical Process.myUid compatibility; that is no authority.

ProcessStarter now receives the immutable install identity, retained in
PendingProcess. SystemServer supplies its assigned UID to the existing honest
shim environment. That environment field and the wire UID remain forgeable;
neither authorizes broker admission. An uninstall racing an already captured
launch identity still needs registration/revocation coordination.

## Implemented native instance lifetime and missing authority protocol

Generic native Spawn creates children suspended, captures creation time from the
actual CreateProcess HANDLE, inserts that HANDLE/birth into its local child map,
and then resumes the initial thread. Failure attempts to terminate its owned
child and closes the handles. Owned StartTime and Kill use the retained HANDLE;
ExitCode closes/consumes it under the same mutex. Unknown/adopted process queries
still use PID/OpenProcess and provide no authenticated launch record. No new
public registration selector or claimed role was introduced.

The local child map is NOT the broker registry. The missing protocol is still:
launcher creates private broker control capability restricted to an explicit
PROC_THREAD_ATTRIBUTE_HANDLE_LIST; bootstrap designates actual retained launcher,
servicemanager and system-server instances; only the designated authority can
publish package/install-instance/UID plus retained suspended child HANDLE/birth;
broker acknowledgement must precede ResumeThread, with failure terminating that
child. Descendants/generic public spawn must receive no registration rights.
Broker admission must challenge the live connecting instance using a private
per-launch capability bound to that record, eliminating the PID-query/OpenProcess
reuse gap, and derive UID from the server record. Install replacement, uninstall,
authority death and child exit must revoke records. Client hello cannot create,
override or revive a record. Unknown/unregistered admission must fail closed.

None of that private broker control/bootstrap/challenge/revocation protocol is
implemented here. Binderd and binder wire are unchanged in this follow-up;
unknown clients still receive the existing compatibility admission. The scope
remains an explicitly incomplete prerequisite. Same-user Windows processes can
inject/read capabilities or duplicate handles; no Android OS sandbox or hostile
host-user resistance is claimed by either this prerequisite or the future protocol.

## Fixture source, preservation and exact remaining review

New PackageIdentityFixture uses real two-package installs, changed update APK,
registry reload, uninstall/reinstall, exact UID lookup and unsupported-user
assertions in an empty owned data directory. It requires exact rebuilt app and
framework classes plus real APKs; it is not wired into CI or compiled/run here.
It proves no Binder transport property. Existing raw-pipe fixture is unchanged:
real hello, forged live-child PID with claimed app/system UID, foreign thread
with deliberately shared key, honest transport controls and stale key closure.

Still owed as DISTINCT transport controls: own real hello with forged UID, wrong
birth, terminated launch, stale/reinstalled install incarnation, wrong install
instance, unregistered instance rejection, and honest registered package/system
controls. These must use actual controlled processes and the private registration
path once implemented; no self-asserted fixture role can substitute. The excluded
ACK-loss/public WriteRead-prefix relay was not revived or run.

Performed only source inspection, diff checks and clean local Git checkpointing.
No native/Java/C# compile/configure, fixture execution, runtime test, CI, push,
GitHub write, artifact download, clone or cleanup occurred. Original nine ordinary
SG/malformed/FD rollback contexts and Native broker thirteen mode assertions and
drivers/workflows remain source-unchanged; that is diff scope, not regression
evidence. Qualified primary/1157 history and secondary AOT leaf are unchanged.

Peer questions: verify pinned framework API signatures/getSharedUserId, allocator
range/reuse policy, migration failure behavior and atomic replacement; inspect
UID reservation across failed parses and command-line overrides; review native
handle close/resume failure paths. Then implement/review private bootstrap and
live capability challenge before evaluating transport admission or OriginalCallerUID
security. Do not patch AOT around the still untrusted wire UID.
