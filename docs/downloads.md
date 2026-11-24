# DownloadManager

`android.app.DownloadManager` is a client of the `downloads` content provider. On a
device that provider is AOSP's DownloadProvider (`android.process.media`); Gesher
serves the same provider from the system server, so a download outlives the process
of the app that requested it and continues after Gesher restarts.

Reference: android-16.0.0_r4 `packages/providers/DownloadProvider`
(`DownloadProvider`, `DownloadThread`, `DownloadInfo`, `Helpers`, `RealSystemFacade`)
and `frameworks/base` `DownloadManager`, `Downloads`.

## Code

| Class | Role |
| --- | --- |
| `DownloadProvider` | The registered `ContentProvider` (authority `downloads`), its manifest declaration, Binder caller identity, permissions, URI grants, completion broadcasts, network state and per-package network security policy. |
| `DownloadRequests` | DownloadProvider's rows and rules: schema (version 114), insert validation for the public API, `SQLiteQueryBuilder` strict queries with the per-UID clause, update, delete, `openFile` checks, destinations and file names. |
| `DownloadQueue` | What DownloadProvider asks JobScheduler for: runs ready rows when their network, charging and idle requirements and retry delay are met, stops a running row whose charging or idle requirement is lost, reschedules retries, resumes rows on start. |
| `DeviceConstraints` | When JobScheduler's charging, battery-not-low and idle constraints hold, from Windows' power status and display-off inactivity; shared with `JobSchedulerService` so both agree. |
| `DownloadTransfer` | DownloadThread's HTTP execution: redirects, statuses, `Range`/`If-Match` resumption, durable progress checkpoints, final destination handling. |

`ContentProviders.getContentProvider` answers `downloads` with the system server's
provider, and an app cannot publish that authority.

## Contract

- URIs: `my_downloads`, `my_downloads/#`, `all_downloads`, `all_downloads/#`, their
  `/headers` children, and the legacy `download` alias. `public_downloads` is not
  matched by the android-16 provider and is not served.
- Permissions, from the provider's manifest: `INTERNET` for `/my_downloads` and
  `/download`, `ACCESS_ALL_DOWNLOADS` for `/all_downloads`, enforced by the framework's
  `ContentProvider.Transport`. Insert follows `checkInsertPermissions`: public-API
  destinations only, `VISIBILITY_HIDDEN` only with `DOWNLOAD_WITHOUT_NOTIFICATION`
  (a normal permission declared by DownloadProvider, granted when requested), and
  no columns outside the public API.
- Ownership: `my_downloads` is limited to rows whose `uid` or `otheruid` is the
  caller. The requester receives a read/write grant on `all_downloads/<id>`, again
  for every row when the provider starts, which is how
  `getUriForDownloadedFile` URIs open and can be shared.
- Queries use `SQLiteQueryBuilder` with strict mode, strict columns, strict grammar
  and AOSP's projection map, so `DownloadManager.Query` selections and aliases
  (`local_filename`, `media_type`, `total_size`...) are evaluated by SQLite as on
  Android.
- Destinations: `setDestinationUri`, `setDestinationInExternalFilesDir` and
  `setDestinationInExternalPublicDir` are file URIs on the shared volume
  (`Environment.getExternalStorageDirectory()`, `$ANDROID_DATA/media/0`), checked with
  `checkDestinationFilePathRestrictions` against that volume. The default destination
  is the provider's private cache (`$ANDROID_DATA/system/downloads/files`, moved to
  `.../cache` on success), readable through `openFile`. File names and collisions follow
  `Helpers.generateSaveFile` (`name-1.ext`, ...).
- `addCompletedDownload` checks the recorded MediaStore owner before accepting an
  existing path. The internal lookup includes pending and trashed rows without
  filtering owner names for package visibility. The owner must belong to the caller's
  UID (including a shared UID); lookup errors reject the request. Before a media
  provider is installed, there are no media ownership records to check.
- Transfer: the system server's `HttpURLConnection`, which uses Windows' proxy
  settings (`WindowsProxy`) and the Windows trust store. Cleartext and TLS trust come
  from the requesting package's network security config (`ManifestConfigSource`,
  `ApplicationConfig`), as `RealSystemFacade` does. Redirects (5 at most), retryable
  statuses, 30 s exponential backoff and `Retry-After` follow DownloadThread and
  DownloadInfo.
- Networks: the Windows default network, when it reaches the internet, decides
  `setAllowedOverMetered`, `setAllowedOverRoaming` and Wi-Fi-only requests
  (`getRequiredNetworkType`, including the over-mobile size settings). Without a
  suitable network nothing is requested and the row stays pending until the network
  changes. A cost or roaming change also rechecks a transfer already in progress;
  when its network becomes unsuitable it keeps its partial file and waits, then
  resumes with `Range` when an allowed network returns.
- Charging and idle: `setRequiresCharging` and `setRequiresDeviceIdle`
  (`FLAG_REQUIRES_CHARGING`, `FLAG_REQUIRES_DEVICE_IDLE`, which `Helpers.scheduleJob`
  passes to `JobInfo`) are evaluated exactly as Gesher's `JobSchedulerService`
  evaluates those job constraints (`DeviceConstraints`): charging is AC power with a
  battery that is charging, full or absent (a PC without a battery on mains) and not
  low; idle is the session display off without input for
  `config_jobSchedulerInactivityIdleThreshold` (31 minutes). An unknown power status
  satisfies neither. A waiting row stays `STATUS_PENDING`; Windows' power status,
  display and resume notifications (and, while the display is off, one check when the
  threshold is reached) evaluate it again. A running download whose requirement is
  lost stops as JobScheduler's `onStopJob` makes `DownloadThread` stop: it keeps its
  bytes, waits to retry, and resumes once its retry delay and requirements allow.
- Completion: a completed row (success or error), or the removal of an unfinished one,
  sends `ACTION_DOWNLOAD_COMPLETE` with `EXTRA_DOWNLOAD_ID` to the requesting package
  (legacy rows: `Downloads.Impl.ACTION_DOWNLOAD_COMPLETED` to their class).
- Persistence: `$ANDROID_DATA/system/downloads/downloads.db`. A process without
  persistent device state (gesher-app's own services) keeps an in-memory database.
  Rows of uninstalled UIDs are removed with their files.

## Not implemented

These are absent rather than simulated:

- Notifications: `setNotificationVisibility` is validated and stored, but no download
  notification is posted, so `ACTION_NOTIFICATION_CLICKED` is never sent.
- MediaStore scanning: completed downloads are not scanned automatically and
  `mediaprovider_uri` stays null. Existing media ownership is still enforced.
- Certificate transparency policy (`ConfigNetworkSecurityPolicy`) is not installed per
  download; the package's trust anchors and cleartext rules are.
- Disk-space preallocation is a usable-space check only.
- The downloads UI (`ACTION_VIEW_DOWNLOADS`) and `DownloadStorageProvider`
  (`com.android.providers.downloads.documents`) are not provided.

## Tests

`tools/test_download_manager_contracts.py` compiles `DownloadRequests`,
`DownloadQueue`, `DownloadTransfer`, `DeviceConstraints` and `SharedStorageOwnership` unchanged with AOSP's `SQLiteQueryBuilder` and
`SQLiteTokenizer` (pinned android-16.0.0_r4 sources), runs SQL in SQLite through
sqlite-jdbc, and transfers over real sockets from a local HTTP server: admission,
ownership, strict SQL, redirects, HTTP failures, cut connections with resumption,
network requirements, charging and idle requirements (from a stand-in power status and
display-off time), cleartext refusal, file naming, removal and restart recovery.
It runs on Linux and on a Windows JVM (`windows-host-contracts`), where canonical
paths use backslashes; the path rules compare whole path components, never a `/`
prefix. `DownloadProvider` (Binder, permissions, grants, Windows networks and proxy) is
not exercised by that host test.
