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
| `DownloadQueue` | What DownloadProvider asks JobScheduler for: runs ready rows when their network requirement and retry delay are met, reschedules retries, resumes rows on start. |
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
  changes.
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
- MediaStore: Gesher has no MediaStore provider, so completed downloads are not
  scanned, `mediaprovider_uri` stays null, and `addCompletedDownload` does not check
  MediaStore ownership.
- Certificate transparency policy (`ConfigNetworkSecurityPolicy`) is not installed per
  download; the package's trust anchors and cleartext rules are.
- `setRequiresCharging` / `setRequiresDeviceIdle` flags are stored but do not delay a
  download; disk-space preallocation is a usable-space check only.
- The downloads UI (`ACTION_VIEW_DOWNLOADS`) and `DownloadStorageProvider`
  (`com.android.providers.downloads.documents`) are not provided.

## Tests

`tools/test_download_manager_contracts.py` compiles `DownloadRequests`,
`DownloadQueue` and `DownloadTransfer` unchanged with AOSP's `SQLiteQueryBuilder` and
`SQLiteTokenizer` (pinned android-16.0.0_r4 sources), runs SQL in SQLite through
sqlite-jdbc, and transfers over real sockets from a local HTTP server: admission,
ownership, strict SQL, redirects, HTTP failures, cut connections with resumption,
network requirements, cleartext refusal, file naming, removal and restart recovery.
`DownloadProvider` (Binder, permissions, grants, Windows networks and proxy) is not
exercised by that host test.
