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
  may change its connection. Caller authentication is captured before the
  providers' monitor; token mutation checks the client's PID, UID and live
  process generation inside that monitor. A physical-death marker prevents
  a lookup authenticated before death from recreating connections after
  cleanup, without changing the intentional-exit service restart policy.
  Server-local acquisitions (getType, `getContentProviderExternal`) stay
  uncounted.
- **An idle reaper ends cached processes.** Every min(60 s, idle time / 4)
  the system server of a persistent device looks at the processes it spawned that have no
  activity and no pending work, and ends (`scheduleExit`, the same path as
  `maybeExit`) one that is not in use
  (`ActivityManagerService.inUse`) and was not used for
  `GESHER_CACHED_PROCESS_IDLE_S` seconds (600 by default; 0 turns it off).
  Positive values too large to convert to milliseconds saturate at
  `Long.MAX_VALUE`, preserving a long idle timeout instead of wrapping
  to an immediate exit.
  In use means: a service still being brought up, with a restart pending,
  started or foreground, or bound by another process or by the system (a
  running job, a sync); a broadcast in flight for it; or a client holding a
  stable or unstable provider reference. A binding from the process itself
  and a client that released its providers do not count. Use is stamped on
  the process (`ProcessRecord.lastActive`) when work is dispatched to it,
  when something in it ends (`maybeExit`), on provider acquisitions and
  reference changes, on a bind to one of its services, on an admitted
  registered broadcast, and whenever a sweep finds it in use; an atomic
  maximum makes concurrent stamps move only forward. Ready-process work is
  stamped and its dispatch decision is captured under ATM.
  The final retirement acquires the monitors in this order:
  ActiveServices, BroadcastController, ATM, ContentProviders. It rechecks
  service and receiver admission, process/activity/pending-work ownership,
  the idle stamp, and provider references before committing the exit flags
  and removing the published providers. A failed check never exposes an
  exiting flag. Before releasing service admission, it also removes the
  retired process's unused/self-bound service records and connections.
  A real binding admitted first therefore prevents retirement; one admitted
  afterwards gets a fresh record and cannot receive the old published
  Binder. Old process death cleanup cannot remove that replacement record.
  The nested section contains checks, flags and registry changes only;
  `scheduleExit` and other application callbacks run after all monitors are
  released. Unordered registered callbacks use `assumeDelivered` and have
  no completion acknowledgement; admission refreshes their normal idle
  grace instead of retaining an unbounded receiver record.
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
- The idle reaper does not commit an exit while a tracked external/system
  binding, started service, pending receiver or held provider reference
  keeps the process in use. A request racing admission either prevents the
  commit or sees the committed exit and targets a replacement as applicable.
  Failed retirement leaves the live process and its provider publication
  intact; there is no temporary exiting state.
- Holders handed out to the host or uncounted (`getContentProviderExternal`,
  getType's server-local lookup, the system server's own callers) take no
  reference: such a user does not keep the provider's process from being
  reaped, and the next call from it after a reap finds a dead Binder
  (it asks again and gets the new process).
- A Binder obtained from a service that only its own process binds (passed
  on to another process through a callback or an intent, not through
  bindService) is not a use the server sees: the reaper may end that process
  under the holder, which then gets a DeadObjectException, as on Android
  when the low-memory killer ends a cached process.
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
- `ProviderSecurityTest` covers unstable/stable counts, duplicate holder
  removal, stable-to-unstable conversion, removal/reacquisition and client
  death. Added regressions cover PID/UID token ownership, live and stale-stamp
  retirement, the ATM/provider authentication lock order, and a lookup
  delayed across actual process-death cleanup.
- `SystemBindingCleanupTest` covers a running job's system binding, cached
  self-bindings, the idle grace, a same-service bind during the exit callback,
  old-generation cleanup after that bind, registered-broadcast admission,
  and concurrent activity stamps. Its exit callback checks that none of
  the four controller monitors is held.
- The revised code and tests require the PR's Windows CI result. The review
  prepared these regressions from source; it did not run the Windows runtime
  or reproduce the device observations below.
- The original PR author reported the initial reference-count/reaper tests
  passing and the following microG device run (official GmsCore, GsfProxy,
  the Play Store; no account):
  Waze launched and closed, the Play Store launched and closed (DroidGuard
  starts in `.unstable`), Waze launched again. Before, `.unstable` stayed for
  the rest of the 26-minute run (427 MB working set, 3 GB commit on the
  pre-0080 runtime used). With `GESHER_CACHED_PROCESS_IDLE_S=180`, it was
  reaped 215 s after its last use, started again by DroidGuard's next bind
  when the Play Store was opened again, and reaped again; GMS main and
  `:persistent` stayed; no DeadObjectException or provider error in any log,
  the same provider lookups failing as before.
