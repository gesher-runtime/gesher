# Pinned URI permission oracle

`GrantUri.java`, `UriPermission.java`, and `UriPermissionOwner.java` are unmodified
Apache-2.0 AOSP sources from `platform/frameworks/base` commit
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:

https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/uri/

Verified Git blob IDs:

| Source | Git blob |
| --- | --- |
| GrantUri.java | f9b6a7a81deeee1844cd4f53f6cdee0e0b025406 |
| UriPermission.java | 917874b3da2a90bcdf1e5fc8ce2a980dd52595a7 |
| UriPermissionOwner.java | b5e7cb0577178456a0426595373b52c6185c8177 |

`AospUriPermissionOracle` exposes their real transitions to the comparison test.
The small `UriGrantsManagerInternal` interface supplies only the notification
boundary after an owner removes a membership; it implements no permission
policy. Native logging and platform initialization use the existing explicit
host adapter. No external Binder owner token is allocated by this oracle.

Run `tools/test_persistable_uri_grants.py` with the framework/core/app class paths,
apksig and a JDK. It compares production mode and owner transitions with AOSP and
then exercises the production grant service, parser, persistence, shared-UID
lookup and owner lifetimes. Its controlled Binder identities and in-memory APK
metadata do not establish Windows/ART, actual package installation, or remote
Binder transport coverage. Windows track 94 separately runs the service and
Intent owner fixtures with native Binder tokens.
