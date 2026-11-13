# 0082 — Cached app processes: provider reference counts and an idle reaper

**Status:** accepted, 2026-11-13. Extends ADR 0024 (services in the app
process) and ADR 0080 (memory footprint).

## Context
With microG enabled, a device kept five background processes for good:
`com.google.android.gms`, `gms:persistent`, `com.google.android.gms.unstable`
(DroidGuard), `com.android.vending` and `vending:instant_app_installer`, each
80–120 MB of working set. The system server ends a process only from
`maybeExit`, as soon as nothing keeps it (`ActivityManagerService.keepsAlive`):
no activity, no service record, no receiver in flight, no provider client.
Android instead keeps an unused process *cached* and the low-memory killer
ends cached processes when memory is needed; Gesher has no low-memory killer,
so whatever `keepsAlive` counts lives as long as the device. Read from a live
device's `system_server.log`:

1. **Every GMS process was kept.** `keepsAlive` returned true for the GMS
   package, so `.unstable` stayed after DroidGuard's last service was stopped.
2. **A service bound by its own process kept the process.** The Play Store's
   main process bound its own `ProfileStateService` (BIND_AUTO_CREATE) once
   and never unbound it; `hasServices` counted that service, so the process
   stayed although no other process used it. Android's OomAdjuster ignores a
   binding to oneself.
3. **Provider clients were never released.** Every `ContentProviderHolder`
   said `noReleaseNeeded`, `refContentProvider` and `removeContentProvider`
   did nothing, and a client counted until its process died; a client that
   never dies (GMS) kept every provider it once queried.

## Decision
- **Provider connections are reference counted, as ContentProviderHelper
  does.** `getContentProvider` from an app's ActivityThread for another
  process's published provider returns a holder with a
  `ContentProviderConnection` (`ContentProviders.Connection`, one per client
  process and provider) and `noReleaseNeeded = false`; each acquisition adds a
  stable or unstable reference. ActivityThread's own counting then calls
  `refContentProvider` (first stable/unstable reference, the stable-to-unstable
  switch while a removal is pending) and, one second after its last release
  (CONTENT_PROVIDER_RETAIN_TIME), `removeContentProvider`; a connection with
  no reference left is dropped. Only the client's own process (or the system)
  may change its connection. Server-local acquisitions (getType,
  `getContentProviderExternal`) stay uncounted.
- **An idle reaper ends cached processes.** Every minute the system server
  of a persistent device looks at the processes it spawned that have no
  activity and no pending work, and ends (`scheduleExit`, the same path as
  `maybeExit`) one that is not in use
  (`ActivityManagerService.inUse`) and was not used for
  `GESHER_CACHED_PROCESS_IDLE_S` seconds (600 by default; 0 turns it off).
  In use means: a service still being brought up, with a restart pending,
  started or foreground, or bound by another process or by the system (a
  running job, a sync); a broadcast in flight for it; or a client holding a
  stable or unstable provider reference. A binding from the process itself
  and a client that released its providers do not count. Use is stamped on
  the process (`ProcessRecord.lastActive`) when work is dispatched to it,
  when something in it ends (`maybeExit`), on provider acquisitions and
  reference changes, and whenever a sweep finds it in use.
- **Only microG's main and `:persistent` processes are persistent**
  (`ActivityManagerService.isPersistent`): the main process hosts the
  accounts, location and the chimera provider every client asks for, and
  `:persistent` keeps Cloud Messaging's MCS connection and check-in. Other
  GMS processes (`.unstable`, `:ui`) are reaped like any app's.
- **`maybeExit` is unchanged.** It still ends a process at once when
  `keepsAlive` finds nothing (the services track relies on that), and still
  keeps GMS and pinned provider clients; the reaper is a second, later and
  stricter policy, so nothing ends sooner than before except through it.
- Whatever needs a reaped process again starts a new one: a service bind or
  start, a provider acquisition (`getContentProvider` waits for the new
  process to publish), a broadcast.

## Consequences
- A process ends only when no other process could notice: no live binding
  from another process, no held provider reference (a client never gets a
  DeadObjectException for a stable reference it holds). It may still race a
  new request in the moment it is told to exit, as with `maybeExit`; the
  request then starts a new process.
- The Play Store and DroidGuard start again when needed, a few seconds each
  time instead of not at all; 10 minutes of idleness keeps the frequent ones
  (DroidGuard during an integrity check burst) warm.
- `vending:background` and `quick_launch` still start twice in a row now and
  then (a job ends and `maybeExit` ends the process, the next job starts it
  again within seconds). A short grace period in `maybeExit` would avoid it,
  but changes the immediate exit the services track asserts; not done.
- Not done: Android's importance propagation (a cached client does not make
  its bound service cached), a cached-process LRU by memory pressure.

## Verification
- `ProviderSecurityTest` (connection counts: unstable, stable, the duplicate
  holder's removal, stable-to-unstable, removal, a new connection after it,
  client death) and `SystemBindingCleanupTest` (the reaper keeps a process a
  running job binds, ends one only its own binding keeps, not before its idle
  time) pass on the Windows runtime.
- A microG device (official GmsCore, GsfProxy, the Play Store; no account):
  Waze launched and closed, the Play Store launched and closed (DroidGuard
  starts in `.unstable`), Waze launched again. Before, `.unstable` stayed for
  the rest of the 26-minute run (427 MB working set, 3 GB commit on the
  pre-0080 runtime used). With `GESHER_CACHED_PROCESS_IDLE_S=180`, it was
  reaped 215 s after its last use, started again by DroidGuard's next bind
  when the Play Store was opened again, and reaped again; GMS main and
  `:persistent` stayed; no DeadObjectException or provider error in any log,
  the same provider lookups failing as before.
