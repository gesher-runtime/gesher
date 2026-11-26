# MediaStore provider (`content://media`), first slice

Apps save photos, recordings and downloads through `MediaStore`: they insert a row
into a collection, write the file through the returned URI, and publish it; galleries
and players query the collections; apps that write files directly ask the media
scanner to index them. Gesher had no provider for the `media` authority, so all of
that failed. This slice serves `content://media` from the system server over the
shared volume `$ANDROID_DATA/media/0` (the directory `StorageService` reports as the
primary emulated volume, i.e. what `Environment.getExternalStorageDirectory()` returns
in apps), following AOSP's MediaProvider module at `android-16.0.0_r4`.

## Before and after, as an app observes it

| App operation | Before (upstream `main`) | After |
|---|---|---|
| `resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)` | `IllegalArgumentException: Unknown URL content://media/external/images/media` (ContentResolver.java:2207, no provider) | the item URI `content://media/external/images/media/<id>`; the row is owned by the caller |
| `resolver.openFileDescriptor(item, "w")`, write, close | `FileNotFoundException`: no provider | a descriptor on `<volume>/<RELATIVE_PATH>/<name>` (`.pending-<expires>-<name>` while pending); on close `_size`, `date_modified` and, for PNG/JPEG/GIF/BMP/WebP, `width`/`height` are read from the file |
| `resolver.update(item, IS_PENDING=0)` | `IllegalArgumentException` | the file is renamed to its display name and the row becomes visible to other readers |
| `resolver.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, ...)` | `null` cursor | the caller's own published rows; every published image with `READ_MEDIA_IMAGES` (API 33+) or `READ_EXTERNAL_STORAGE` (below 33) |
| update/delete of another app's image | n/a | `SecurityException("<pkg> has no access to <uri>")` unless the caller is a legacy writer or holds `MANAGE_EXTERNAL_STORAGE`, or a URI grant covers it |
| `MediaScannerConnection.scanFile(context, paths, ...)` | callback with `null` URI for every path (`MediaStore.scanFile` was a placeholder that returned `null`) | the file is indexed with metadata read from it and the callback gets its `content://media/external_primary/{images,video,audio}/media/<id>` or `.../file/<id>` URI |
| `sendBroadcast(ACTION_MEDIA_SCANNER_SCAN_FILE, file://...)` | nothing | the file is scanned as above |
| `MediaStore.getVersion` / `getGeneration` | constant `"0"` / `0` | `1602:<database uuid>` (opaque per-UID digest for API 36 targets) / a generation that grows with each change and survives restarts |
| `MediaStore.createWriteRequest` / `createDeleteRequest` / trash / favorite | `IllegalArgumentException: Unknown URL` (no provider) | arguments validated as AOSP does, then `UnsupportedOperationException("Unsupported call: create_write_request (no media consent screen on this device)")`; no `PendingIntent` and no grant is fabricated |

## Where it lives

- `app/java/com/gesher/server/MediaStoreIndex.java`: the index. AOSP's `files` table
  (the full `DatabaseHelper.createLatestSchema` column list) in SQLite, the Images,
  Video, Audio, Downloads and Files collections over it, the access rules, placement
  rules, pending lifecycle, scanner, orphaning and expiry. No Binder or Windows
  dependency, so the host harness runs it unchanged.
- `app/java/com/gesher/server/MediaStoreProvider.java`: the `ContentProvider` in the
  system server. It authenticates the Binder caller (as `DownloadProvider` and
  `SettingsProvider` do), maps the caller's permissions, app ops, target SDK, URI
  grants and package visibility into `MediaStoreIndex.Caller`, implements `call`
  (`scan_file`, `get_version`, `get_generation`, `create_*_request`), and opens file
  descriptors (`ParcelFileDescriptor.open` with a close listener that re-reads the file).
  Its `ProviderInfo` mirrors the module manifest: exported, `grantUriPermissions`,
  `forceUriPermissions`, no read/write permission (the provider checks per row).
- Hosting: `ContentProviders.setMedia` plus a `getContentProvider` branch, the same
  pattern as the documents provider and as PR #416's `downloads` provider (not a
  dependency). Registering the authority makes `PackageManager.resolveContentProvider("media")`
  answer it and makes an APK's attempt to publish `media` fail, through the existing
  registered-provider checks.
- `SharedStorageOwnership` supplies a trusted internal lookup for providers accepting
  existing shared-volume files. It includes pending and trashed rows and does not
  filter owners for package visibility. DownloadManager uses it to require that a
  recorded owner belongs to the caller's UID; lookup errors reject the request.
  Provider startup installs an unavailable lookup before opening the database and
  replaces it only after successful registration and startup, so an index failure
  cannot be mistaken for a runtime without MediaStore.
  The hook has no dependency on DownloadManager, so either provider can be built alone.
- `Services.create` builds it with its database at `$ANDROID_DATA/system/media/external.db`
  (in memory when the device keeps no persistent state, as the accounts and downloads
  stores do).
- `BroadcastController.addSystemReceiver`: a server-local receiver for implicit,
  package-less broadcasts of one action. AOSP's `MediaReceiver` is an exported
  manifest receiver without a permission, so any sender reaches it.
- `app/java/android/provider/MediaStore.java` (the boot class path placeholder for the
  module class): `scanFile`, `getVersion` and `getGeneration` now use the provider's
  call protocol exactly as the module's `MediaStore` does.

## The contract, with the AOSP lines it follows

Sources: `msft-mirror-aosp/platform.packages.providers.mediaprovider` and
`msft-mirror-aosp/platform.frameworks.base`, tag `android-16.0.0_r4`.

**Ownership and collection access** (`AccessChecker.java`). Without collection
permission an app sees its own rows:

```java
// AccessChecker.getWhereForConstrainedAccess, line 290
            case IMAGES_MEDIA_ID:
            case VIDEO_MEDIA_ID:
            case VIDEO_MEDIA: {
                return getWhereForOwnerPackageMatch(callingIdentity);
```

Audio adds `OR is_ringtone=1 OR is_alarm=1 OR is_notification=1`; Downloads and Files
add `volume_name='external_primary'` for legacy writers, and Files adds each media type
the caller may access. Downloads and Files collections are fully readable only for
legacy readers:

```java
// AccessChecker.hasAccessToCollection, lines 132-133
                return !forWrite
                        && callingIdentity.isCallingPackageLegacyRead();
```

**Which permission reads a collection** (`PermissionUtils.java`):

```java
// checkPermissionReadImages, line 326
        String permission = targetSdkIsAtLeastT && SdkLevel.isAtLeastT()
                ? READ_MEDIA_IMAGES : READ_EXTERNAL_STORAGE;
```

then the app op through `checkAppOpAllowingLegacy` (`OPSTR_READ_MEDIA_IMAGES`, allowed
by default). Writing others' media (`checkPermissionWriteImages`) needs
`OPSTR_WRITE_MEDIA_IMAGES`, which is `MODE_ERRORED` by default (the system gallery
role holds it), or legacy storage (`OPSTR_LEGACY_STORAGE`) with
`WRITE_EXTERNAL_STORAGE`. `MANAGE_EXTERNAL_STORAGE` gives global access
(`checkCallingPermissionGlobal`). Gesher computes `OPSTR_LEGACY_STORAGE` with
StorageManagerService's rule (target below Q, or below R with
`requestLegacyExternalStorage`), as PR #416 does, and applies
`checkIsLegacyStorageGranted`'s cut-off at API 35.

**Writing another app's item asks for consent.** `MediaProvider.enforceCallingPermissionInternal`
throws `RecoverableSecurityException` whose action is
`MediaStore.createWriteRequest(...)`, a `PendingIntent` to
`PermissionActivity` (line 8784: `new Intent(method, null, context, PermissionActivity.class)`).
Gesher has no such consent screen, so it throws the plain
`SecurityException(getCallingPackageOrSelf() + " has no access to " + uri)` that AOSP
throws when no user grant is possible, and `create_*_request` calls fail after AOSP's
validation (`"All requested items must be referenced by specific ID"`,
`"Invalid column " + key`) with the provider's standard unsupported-call error
(`MediaProvider.call`, line 7457: `throw new UnsupportedOperationException("Unsupported call: " + method)`).

**Strict queries** (`MediaProvider.getQueryBuilderInternal`):

```java
        qb.setStrict(true);
        if (isCallingPackageSelf()) {
            // ...
        } else {
            qb.setTargetSdkVersion(getCallingPackageTargetSdkVersion());
            qb.setStrictColumns(true);   // line 6309
            qb.setStrictGrammar(true);
        }
```

Gesher uses the framework's `SQLiteQueryBuilder` (the module's copy adds only
`setTargetSdkVersion` and `allowColumn`). Pending and trashed items are excluded by
default (`defaultMatchForPendingAndTrashed = MATCH_EXCLUDE;`, line 6338), included for
item URIs and for `QUERY_ARG_MATCH_PENDING`/`includePending=1`. For API 34+ callers
without `QUERY_ALL_PACKAGES`, `owner_package_name` is `NULL` for packages the caller
cannot see (`isApplicableForOwnerPackageNameFiltering`).

**Placement** (`MediaProvider.ensureFileColumns`): images only under `DCIM`/`Pictures`,
video under `DCIM`/`Movies`/`Pictures`, audio under the music directories, downloads
under `Download`, files under `Download`/`Documents`, an app's own `Android/media/<pkg>`,
and other shared locations for `MANAGE_EXTERNAL_STORAGE`. Other packages'
`Android/data/<pkg>` and `Android/obb/<pkg>` remain excluded, including for managers
and legacy writers:

```java
// line 5053
                throw new IllegalArgumentException(
                        "Primary directory " + primary + " not allowed for " + uri
                                + "; allowed directories are " + allowedPrimary);
```

MIME types must match the collection (`"MIME type ... cannot be inserted into ...;
expected MIME type under image/*"`), names are made unique (`name (1).ext`), a missing
extension comes from the MIME type, and an app targeting API 34+ cannot set `_data`
(`"Mutation of " + column + " is not allowed."`, line 5900).

**Pending items** (`FileUtils.computeDataFromValues`):

```java
// FileUtils.java lines 1464-1465
            final String combinedString = String.format(
                    Locale.US, ".%s-%d-%s", FileUtils.PREFIX_PENDING, dateExpires, displayName);
```

with `DATE_EXPIRES` = now + 7 days. Only the owner opens a pending item
(`"Only owner is able to interact with pending/trashed item "`, line 10545). Publishing
(`IS_PENDING=0`) renames the file and refreshes metadata from it, as AOSP's blocking scan
on publish does. Columns other than `sMutableColumns` are writable only while pending.

**Package removal** (`MediaService`: `PACKAGE_FULLY_REMOVED` →
`MediaProvider.onPackageOrphaned`): rows under the package's `Android/media/<pkg>/`
whose file is gone are deleted, and every other row is orphaned:

```java
// MediaProvider.orphanEntries, line 2435
        values.putNull(FileColumns.OWNER_PACKAGE_NAME);
```

Pending items left behind expire: `deleteOrExtendExpiredItems` (line 2180:
`String selection = FileColumns.DATE_EXPIRES + " < " + now;` and
`IS_PENDING=1 OR IS_TRASHED=1`) deletes items that expired within the last week and
extends older ones by a week. AOSP runs it in idle maintenance; Gesher runs it when the
provider starts and after each package removal.

**Moves, deletes and failures.** Placement columns (`_display_name`, `relative_path`,
`mime_type`, `is_pending`, `is_trashed`) move a file only for a single item, as
`MediaProvider.updateInternal` (line 9370) allows; a collection update that changes them
throws `IllegalArgumentException: Movement of <uri> which isn't part of well-defined
collection not allowed` before anything moves. An actual move takes a unique
destination (`ensureUniqueFileColumns`, line 9440), so renaming an item onto an existing
name gives `name (1).ext`. AOSP renames the file (`Os.rename`, line 9463) and then
writes the row; when the row write fails or the process dies in between, Android's
scanner reconciles later (`ModernMediaScanner.reconcileAndClean` deletes rows whose file
it did not find; the moved file is scanned as a new, ownerless item). Gesher has no
full volume scan, so the index keeps the two together itself: every file move or
delete is first appended to a journal in `external-file-ops/` next to the database and
forced to disk; when the transaction does not commit, completed moves are reversed;
the journal is removed after the commit. A journal found when the provider starts
means the system server stopped in between: a move whose row still names the old path
is undone (the item keeps its id, owner and pending state), and a delete whose row
still names a missing file completes by deleting the row. This covers rename/move,
pending -> published, trash/untrash, `delete` and pending/trashed expiry.

**Scanning** (`MediaProvider.getResultForScanFile`, line 7802:
`res.putParcelable(Intent.EXTRA_STREAM, uri);`; client `MediaStore.scanFile`:
`resolver.call(AUTHORITY, SCAN_FILE_CALL, file.getAbsolutePath(), null)` then
`out.getParcelable(Intent.EXTRA_STREAM)`). `ModernMediaScanner.scanFile` returns the
item URI in its media collection (`getFirstResult`), `null` for a file it does not
index. Gesher follows `shouldScanDirectory` (`FileUtils.PATTERN_INVISIBLE`:
`Android/data|obb|sandbox`, `.transforms`, `.picker_transcoded`,
`Movies|Music|Pictures/.thumbnails` are never scanned), hidden names and `.nomedia`
directories (indexed as `MEDIA_TYPE_NONE`), never-hidden default top-level
directories, `MimeUtils.resolveMediaType`, and deletes the row of a scanned file that
no longer exists. A scanned row keeps its owner if it had one and has none otherwise.

**Location metadata.** AOSP redacts location for a reader that is not the owner, not
a manager and lacks `ACCESS_MEDIA_LOCATION`
(`isRedactionNeededForOpenViaContentResolver`), and refuses an original
(`"Caller must hold ACCESS_MEDIA_LOCATION permission to access original"`, line 10715).
Gesher has no redactor, so it opens such a file only when it can show it carries no
location metadata (JPEG EXIF without GPS IFD and no XMP location, PNG without
`eXIf`/XMP chunks, GIF, BMP, WebP without EXIF/XMP chunks, ISO media whose `moov`/`meta`
has no `©xyz`/`loci`/GPS boxes); otherwise it throws that `UnsupportedOperationException`
with an explanation. It never hands out unredacted location.

**Version and generation** (`getResultForGetVersion`, line 7851:
`return db.getVersion() + ":" + dbUuid;`; for API 36 targets
`LOCKDOWN_MEDIASTORE_VERSION` makes it an opaque per-UID hash, here a SHA-256 prefix
instead of farmhash, which the API leaves opaque).
Generation advances for row deletion too, including scanner cleanup, expired items,
package removal and journal recovery. A row deletion and its generation update commit
together. Retained items whose owner or expiry changes also receive a new
`generation_modified`, so a client synchronizing by generation sees those changes.

## Windows mapping

The volume is the real directory `%ANDROID_DATA%/media/0`; `_data` is the path an app's
`java.io.File` uses for it (Gesher's libcore uses `/` separators). Relative paths are
computed against that directory rather than AOSP's `/storage/emulated/<user>/` pattern.
Path containment is checked on the normalized absolute path and then on the canonical
path, so `..` segments and links cannot place a row outside the volume.
Before an application request creates a directory, opens a file, moves an item or
deletes it, the index also checks the private-directory boundary on both paths.
Only the caller's own package or another installed package sharing its authenticated
UID can access that package's `Android/data` and `Android/obb`; neither global media
permission nor a row's recorded owner overrides this boundary. Ordinary scoped
owners keep their MediaStore access to their public files. The trusted provider's
own maintenance operations retain their system identity.

## Validation

`python3 tools/test_media_provider_contracts.py --java-home "$JAVA_HOME"` (CI: Linux
`artifact-preflight` job, next to the HttpEngine and DNS Java host contracts; Windows:
`windows-host-contracts` on windows-2022 and windows-2025 with Temurin 21, so the same
checks run over NTFS paths with `\` separators and Windows rename/delete semantics) compiles
`MediaStoreIndex` unchanged with AOSP's `SQLiteQueryBuilder`/`SQLiteTokenizer`
(`android-16.0.0_r4`, SHA-256 pinned) and sqlite-jdbc 3.46.1.3 (SHA-256 pinned), and runs
`tests/mediastore/MediaStoreHost.java` over a real database file and real files:

```
MediaStoreIndex SHA256 6e6f0fa98a8e028ffbf9a8183f2110ac9b7c40fb0cf7e50515a6e045f3af0236
PASS  media: insert(RELATIVE_PATH, IS_PENDING=1) -> openFileDescriptor("w") -> update(IS_PENDING=0) places Pictures/Gesher/sunset.png, publishes it, and indexes its real size and 640x480
PASS  media: an app sees and edits its own rows; READ_MEDIA_IMAGES (READ_EXTERNAL_STORAGE below API 33) reads others' images; writing others' needs consent -> SecurityException; legacy writers keep access
PASS  media: MediaProvider placement rules: DCIM/Pictures only for images, MIME family checks, _data rejected for API 34+, unique names, default directories, internal volume unattached
PASS  media: Downloads (Download/, is_download), Audio (READ_MEDIA_AUDIO) and Files collections over one files table
PASS  media: query selection/args, sort and limit through AOSP SQLiteQueryBuilder strict mode; unknown columns/tokens -> IllegalArgumentException, malformed SQL -> SQLiteException, no selection widens access
PASS  media: delete(uri) removes the owner's row and file; others' rows need write access; the owner trashes/untrashes; MANAGE_EXTERNAL_STORAGE has global access
PASS  media: MediaScannerConnection.scanFile / ACTION_MEDIA_SCANNER_SCAN_FILE indexes a file written to the shared volume with its real metadata, no owner, invisible/hidden/.nomedia rules, and forgets deleted files
PASS  media: a file with EXIF GPS is not opened unredacted for a non-owner without ACCESS_MEDIA_LOCATION (Gesher has no redactor); location-free files open normally
PASS  media: createWriteRequest/createDeleteRequest arguments are validated as AOSP does; the provider then reports the missing consent screen instead of returning a PendingIntent
PASS  media: MediaStore.getVersion / getGeneration from the index (generation grows with every change)
PASS  media: the index (rows, owners, generation, version) persists across a system-server restart
PASS  media: package removal orphans published media and drops missing Android/media rows; expired pending items (and their files) are deleted
PASS  media: update moves files only for single items (collection movement -> IllegalArgumentException), picks a unique destination, and reverses every completed move when the transaction fails
PASS  media: a system server killed between a file move/delete and the SQLite commit (rename, pending -> published, delete, expiry) restarts with every row and file consistent
media: 102 checks passed
```

Before the change (`--source-ref upstream/main`), exit status 1:

```
FAIL MediaStore.scanFile does not call the provider: MediaScannerConnection.scanFile reports a null URI for every path
FAIL MediaStoreIndex.java does not exist at upstream/main: no content://media provider, so ContentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, ...) fails with IllegalArgumentException "Unknown URL content://media/external/images/media"
```

`MediaStoreProvider`, `MediaStoreIndex` and the edited `MediaStore` were also compiled
against Robolectric `android-all` 16 (`16-robolectric-13921718-2`, Maven Central), i.e.
the real Android 16 framework API including the hidden members used, with signature
stubs for the Gesher server classes they call. That checks the API use only.

### Limits of this validation

- The Binder glue (`MediaStoreProvider`: caller authentication, permission and app-op
  lookups, `ParcelFileDescriptor` close listener, `call`), the `ContentProviders`
  hosting branch, the broadcast hook and `Services` wiring were not run: there is no
  Windows build or device in this environment. CI's full build and its existing
  provider harnesses (which compile `ContentProviders` and `BroadcastController`
  against the built classes) cover compilation; no end-to-end APK exercises the
  authority yet.
- The harness's callers are stand-ins: they report permissions and app-op modes as
  configured (`READ_MEDIA_*` ops allowed and `WRITE_MEDIA_*` ops errored, as
  `AppOpsManager.opToDefaultMode`); the real lookups are Gesher's `PermissionPolicy`
  and `AppOpsService`.
- `MimeTypeMap` in the harness is a subset of libcore's MimeMap for the fixture types.

## Not implemented (and not reported as available)

- The user-consent UI: `createWriteRequest`, `createDeleteRequest`,
  `createTrashRequest`, `createFavoriteRequest` and the `RecoverableSecurityException`
  action. Writing another app's media therefore needs legacy storage,
  `MANAGE_EXTERNAL_STORAGE`, the write-media app op (system gallery) or a URI grant.
- `READ_MEDIA_VISUAL_USER_SELECTED` partial access (`media_grants`) and Photo Picker
  integration beyond the existing `ACTION_PICK_IMAGES` Windows dialog.
- Location redaction (see above: unverifiable files are refused, not redacted), HEIF
  and transcoding (`getOriginalMediaFormatFileDescriptor` still answers `null`).
- Thumbnails (`Images.Thumbnails`, `Video.Thumbnails`, `loadThumbnail`), album art.
- Audio metadata (artist, album, duration, genres) and video duration/resolution: the
  scanner reads no media containers; those columns stay `NULL` unless the app supplied
  them while the item was pending. Image `datetaken` and orientation are not read from
  EXIF.
- Playlists, albums, artists, genres tables and their URIs; `content://media/<vol>/object`
  (MTP), `media_scanner` and `volumes` URIs; `scan_volume`; directory rows.
- The `internal` volume (system ringtones): it is reported as not attached
  (`IllegalArgumentException: Volume internal not found`, `null` for pre-Q callers).
  Secondary/removable volumes and `QUERY_ARG_INCLUDE_RECENTLY_UNMOUNTED_VOLUMES`.
- FUSE: files an app creates with `java.io` are not indexed until scanned, and a scanned
  file has no owner (on Android, FUSE records the creating app as owner). Such an app
  needs `READ_MEDIA_*` to see the row, or should insert through `MediaStore`. Gesher's
  direct guest-file access has its own external-storage policy checks; this provider
  enforces the corresponding boundary for brokered `content://media` operations.
  The Windows isolation experiment and its limits remain documented in
  [sandbox.md](sandbox.md).
- Trash and favorite as user features: `IS_TRASHED`/`IS_FAVORITE` are stored and an
  owner's trashed item is renamed `.trashed-<expires>-<name>` like a pending one, but
  there is no trash UI.
- `QUERY_ARG_MEDIA_STANDARD_SORT_ORDER`, `EXTRA_HONORED_ARGS`, `QUERY_ARG_RELATED_URI`,
  redacted URIs (`getRedactedUri`), `getDocumentUri`/`getMediaUri`, cloud media
  providers, `MediaStore.getRequireOriginal`, backup/recovery of the database.
- `DownloadManager` (PR #416) does not insert completed downloads into MediaStore yet.

## Overlap check (2026-11-23)

`git ls-remote upstream`, `git log upstream/main -100` and the diffs of every open PR
head from #250 to #416 were searched for `MediaStore`, `MediaProvider`,
`MEDIA_SCANNER`, the `"media"` authority and changes to `ContentProviders.java`. None
implements a MediaStore provider. Related work: #283 (Photo Picker
`ACTION_PICK_IMAGES` over the Windows dialog; unchanged here), #332 (special app access
incl. `MediaStore.canManageMedia`), #338 (content observer dispatch; its notes say it
does not complete MediaProvider), #416 (this fork's `downloads` provider; same hosting
pattern, no dependency), #391/#414 (storage; no MediaStore code).
