# Searchable metadata contract review

The service uses Android's `SearchableInfo.getActivityMetaData` to read the
installed APK. It retains the searchable component and its resource IDs,
suggestion authority, path, selection and action. Suggestion queries continue
through the Android client's ordinary `ContentResolver` and the real provider.

## Android reference

The reviewed reference is AOSP Android 16 release 4, frameworks/base commit
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:

- [Searchables.java](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/search/Searchables.java)
  discovers direct `ACTION_SEARCH` and `ACTION_WEB_SEARCH` declarations, follows
  one activity or application default reference, and filters results using
  `PackageManagerInternal.canAccessComponent`.
- [SearchableInfo.java](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/SearchableInfo.java)
  parses searchable XML and suggestion descriptors. The runtime reuses this
  implementation rather than replacing the schema or executing an invented query.
- [ComputerEngine.java](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/pm/ComputerEngine.java)
  implements component access in terms of installed package visibility.

Package-name equality is not the visibility rule. A manifest-visible package,
`QUERY_ALL_PACKAGES`, a legacy target, or a genuine shared UID can expose a
foreign searchable declaration. The lookup now uses the same `PackageVisibility`
snapshot as the package service. The caller is authenticated before APK access;
visibility, package identity and enabled state are rechecked after resource
loading, so replacing or removing the APK does not publish stale metadata.

## Validation boundaries

`tools/test_searchable.py` exercises the real public `SearchManager`, lookup
service, `ApkPackage` declarations, installed enabled state and package visibility.
Its host fixture constructs parsed manifests and supplies explicit native Binder
identity and XML-reader boundaries. It checks visibility, authentication,
one-hop defaults, direct metadata precedence, unavailable declarations, package
replacement/removal, enabled-state changes, resource exceptions and identity
restoration. The negative-control option restores the original blanket foreign
package denial and must fail the explicit `<queries>` case.

The components and peer APK tests exercise the separate real XML/resource,
cross-process Binder and `ContentResolver` paths on Windows. The peer declares
the components package in `<queries>` and checks that its searchable metadata
is returned. The host fixture does not substitute for those runtime gates.

This remains incremental F34 work. Global-search discovery and selection, the
full package-interaction visibility graph, and end-to-end `SearchView` result
selection are not claimed complete. The existing `PackageVisibility` class
documents the static graph it implements; this change does not fabricate
interaction or installer relationships or claim a host search provider exists.
