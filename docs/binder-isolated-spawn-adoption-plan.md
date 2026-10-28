# Production restricted SpawnOwned adoption checkpoint

Source-only analysis against Binder `92a007578729b65a581a5495ebdca01fea3e82f4`
in the exclusive `wip/binder-isolated-spawn` tree. No production change, CI,
qualified import, or boundary closure is claimed.

The concrete guest route is `SystemServer.java` app-process creation through
`Zygote.spawnPackage`, `services_natives.cc::Zygote_spawn`, and
`services_zygote.cc::SpawnOwned`. The three `StartDevice` calls are trusted
bootstrap actors and must stay explicitly distinct. An environment switch,
command argument or public native policy setter must not select host authority.

The reusable prerequisite at `2496db0d64a527649d67f833955f7b85dc2bd15b`
failed component `40787855371` attempt 1 on both Windows runners, stage 59
before the positive IPC observation. The suspended token/job and bounded
admission/retirement controls passed, but successful isolated child execution
and original denial/resource controls have not been established. Adoption is
paused while the helper's exact child exit and raw pipe diagnostics are added.

## Exact production dependency

The helper accepts an already provisioned SID and accessible executable/resources;
it intentionally performs no profile creation or ACL grants. Current Binder
`CreateNamedPipeW` supplies null security attributes. Current app spawning passes
a command line and install identity, but has no trusted install-owned resource
policy supplying a profile/SID, executable dependencies, APK/runtime read access,
private data write access and a compatible Binder endpoint ACL. Redirecting this
route to an unprovisioned profile would break the actual guest path and would
not qualify isolation. Same-user ACLs cannot establish guest/host separation.

Before connection, define install-generation-owned profile provisioning and
retirement under trusted package authority. Pin executable and required runtime
resources explicitly, allow only intended read/execute or app-data write rights,
and provide the exact private Binder endpoint access for that SID. Never grant
guest access to trusted code/config writes, the writer channel or host commands.
The caller cannot supply arbitrary paths, UIDs, profiles or grants. Revoke and
uninstall must retire exact children before deleting resources/profile; unknown
retirement must retain ownership and report failure.

## Coupled code changes after the prerequisite

1. Add a private owned policy value to SpawnOwned; select it only from the
   trusted package/install route. Preserve explicit trusted bootstrap behavior.
2. Keep existing environment, reserved UID Context and intentional stdio setup.
   Restricted children inherit no bootstrap writer/broker capability. Start
   RestrictedLaunch once with explicit executable/SID and the allowlist; reject
   breakaway and never enter generic CreateProcess after restricted failure.
3. Change Child ownership with process creation: retain the RestrictedLaunch
   owner alongside the exact process/birth/package/install record. Existing raw
   trusted process handles remain separately owned; never close a borrowed
   restricted process handle. Update ExitStatus, Kill, revoke and rollback
   together. Do not transfer the private job out of its owner.
4. Preserve affirmative private publication before Resume, using the actual
   retained handle and birth. Preserve UID/package/generation validation,
   writer leases, cancellation and all existing FD staging/rollback rules.
   Failed publication or resume cancels exact authority, stops the owned job,
   and reports original and retirement errors. Unconfirmed cleanup remains in
   the helper's reserved quarantine and is never considered successful launch.
5. CI must compile actual services_zygote and exercise the actual package call
   route on both Windows versions with profile/resource/Binder positive access,
   trusted resource unchanged, exact access-denied controls, publication/resume
   failure, no writer inheritance, exact lifetime/revoke and cleanup controls.
   Require fresh source/run/attempt/compiler/archive receipts and independent
   source peer before push. A direct helper fixture cannot substitute for this.

CLI/cohost/Main/service/native harness admission, delayed challenge/revoke,
concurrent FD staging, secondary AOT, broad full/package/e2e and release remain
open gates. No assertions are removed and no unrestricted fallback is proposed.
