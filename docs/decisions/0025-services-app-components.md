# 0025 — The rest of the app model in Gesher's system server

**Status:** accepted, 2026-10-23 (services track)

## Context
With the app process attached to the system server
([0024](0024-services-app-process.md)), one launcher Activity went through its
lifecycle with the framework's client code. An ordinary app also starts its
other activities (for results, with launch modes), starts and binds services,
sends and receives broadcasts, and reads content providers, its own and the
system's `settings`. On Android these are ActivityStarter, ActiveServices,
the broadcast queue and ContentProviderHelper in services.jar, out of reach
as before.

## Decision
- **The client side stays the framework's**: Instrumentation,
  `ContextImpl.startService/bindService/registerReceiver/sendBroadcast`,
  `ContentResolver`, `Settings`' NameValueCache and ActivityThread's
  handlers run unchanged; the server answers their AIDL calls and drives the
  app with the IApplicationThread calls and client transactions AMS and ATMS
  use.
- **Activities** (`ActivityTaskManagerService`): the intent is resolved
  explicitly or through the parsed intent filters (`MATCH_DEFAULT_ONLY` for
  starts from apps); the new activity joins the starting activity's task
  (unless FLAG_ACTIVITY_NEW_TASK); the activity in front is paused first
  (`PauseActivityItem`, userLeaving) and the new one launched on
  `activityPaused`; paused activities are stopped once one in front reports
  it resumed or idle. singleTop (or FLAG_ACTIVITY_SINGLE_TOP) at the top of
  the task gets `NewIntentItem`. A finishing activity's result is queued for
  the activity that asked for it; the activity under it is resumed with
  `ActivityResultItem` + `ResumeActivityItem`, then the finished one is
  stopped and destroyed. Not done: singleTask/singleInstance, task
  affinities, `FLAG_ACTIVITY_CLEAR_TOP`, relaunch on configuration change.
- **Services** (`ActiveServices`): `scheduleCreateService`,
  `scheduleServiceArgs` (start ids), `scheduleBindService` per distinct
  intent, `publishService` → `IServiceConnection.connected`,
  `scheduleUnbindService`, `scheduleStopService` when neither a start nor a
  BIND_AUTO_CREATE connection keeps the service. The service's Binder travels
  like any binder object, so a client in its process gets the object `onBind`
  returned. `setServiceForeground` posts to a stand-in `notification` service
  (`NotificationManagerService`: channels and notifications kept and logged,
  nothing shown); foreground rules (types, timeouts) are not enforced.
- **Broadcasts** (`BroadcastController`): registered receivers get implicit
  broadcasts their filters match (`scheduleRegisteredReceiver`); manifest
  receivers get explicit broadcasts, and implicit ones sent to their package
  (`scheduleReceiver`), as Android limits implicit broadcasts to manifest
  receivers. No ordered or sticky broadcasts, no permissions.
- **Content providers** (`ContentProviders`): the package's providers go to
  its process in `bindApplication`, which installs them before
  `Application.onCreate` and publishes them back; `getContentProvider`
  answers published providers and `settings`. **`settings` is a stand-in**
  (`SettingsProvider`): a framework `ContentProvider` in the system server,
  so its Binder is the framework's own transport, holding the System, Secure
  and Global tables in memory with a freshly set up device's values,
  answering NameValueCache's `GET_*`/`PUT_*` calls.
- **Work for a package without a process waits** for one to attach
  (`withProcess`): there is no zygote to start it.
- **What the integration of the graphics track's natives needed on this
  path**: the app process sets the runtime's target SDK from the app's
  before attaching, as the zygote does (ART refuses JNI writes to static
  final fields, which Typeface's font map does, unless the target SDK is
  B or older); and while the window session gives no surface, the app
  process uses no hardware renderer (`ThreadedRenderer.sRendererEnabled`),
  as ActivityThread does for persistent processes on low-end devices
  (`GESHER_APP_HWUI=1` keeps it).

## Consequences
- An app's activities, services, receivers and providers run as on a
  device, through the same client code; what is missing shows up as the
  framework meets it (the stand-in log, `stand-in IFoo.bar not implemented`).
- One process per package, all with the same uid: no permission checks,
  exported or not; a provider of another package is reachable only if its
  process is running.
- A process ends when its last activity is destroyed (as before), even if
  a service of it is still started.
