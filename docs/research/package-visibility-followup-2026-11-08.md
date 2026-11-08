# Package visibility query follow-up — 2026-11-08

This follow-up repairs the public `PackageManager.canPackageQuery` operation.
It implements Android 16's static visibility rules from the current installed
package inventory. It does not claim that Gesher now has Android's complete
package visibility system.

## Confirmed public crash

`PackageManagerService` inherited `IPackageManager.Default.canPackageQuery`,
which returns a null boolean array. The pinned
[`ApplicationPackageManager`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ApplicationPackageManager.java)
single-target overload calls the bulk method and indexes element zero. Asking
whether an installed package can query itself consequently threw
`NullPointerException` in the actual framework wrapper.

The new method returns one boolean per requested target, preserving order and
duplicates. A nonexistent user produces a correctly sized false array before
package lookup. For the supported user, cross-user access is checked before
lookup; the calling UID must be able to see the source and every target. A
missing or caller-filtered package raises
`ParcelableException(PackageManager.NameNotFoundException)`, which the public
framework wrapper converts to its documented checked exception. Empty target
arrays still validate the source. Public null arguments and raw Binder null
array/member behavior retain the pinned framework/service ordering.

These details follow
[`ComputerEngine.canPackageQuery`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java),
including its distinction between the caller's visibility and the source
package's visibility. A caller is allowed to ask about a different source when
both source and targets are visible to that caller.

## Implemented static state

Each call takes one package/removal snapshot. In-progress removals and absent
packages are excluded; replacing or reinstalling a package changes the next
answer without retaining stale manifest relationships. Existing platform and
host capability definitions retain their published identities. No new package,
runtime permission grant, or installed APK is created by a visibility query.

The predicate uses these actual records:

| Rule | Data used |
| --- | --- |
| Core or shared application identity | Actual runtime UIDs, including the legacy mode's shared UID 10000 and protected mode's persistent per-package UIDs. |
| Legacy target | Parsed target SDK below 30, the default boundary of `FILTER_APPLICATION_QUERY`. |
| Query-all declaration | The parsed `android.permission.QUERY_ALL_PACKAGES` request, as upstream does before runtime permission analysis. |
| Explicit package queries | Parsed `<queries><package>` names. |
| Intent queries | Parsed `<queries><intent>` matched by the real framework `IntentFilter.match`, with wildcard support, against exported activities, services, receivers and providers. |
| Provider authority queries | Exact semicolon-separated authority tokens on exported providers. |
| Library providers | Required or optional library names declared by the source and supplied by the target. |
| Permission definers | Permissions requested by the source and declared by the target; visibility is not made symmetric. |
| Force-queryable system packages | Actual system-addon state, parsed force-queryable flag and the loaded framework resource policy. An ordinary app cannot self-promote through a flag or a reserved-looking package name. |
| Static shared libraries | The pinned AppsFilter exception for a named static-library target. |

Manifest relationships are edges between application IDs, not only package
names. Both source and target siblings that actually share a UID contribute
their declarations to the relevant edge. Removing the sibling that supplied an
edge removes that edge from subsequent answers.

Intent visibility does not require an enabled component, a `DEFAULT` category,
or permission to start an exported component. Receiver matching excludes
protected broadcast actions using the installed declarations and real platform
permission catalog. The exclusion applies only to receivers. Matching does not
modify the source intents or installed filters.

Primary sources, all pinned to `android-16.0.0_r4`:

- [AppsFilterBase](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/AppsFilterBase.java): core/shared UID, compatibility, query-all, static library and visibility-edge evaluation.
- [AppsFilterUtils](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/AppsFilterUtils.java): exported components, wildcard filters, protected broadcasts, provider authorities and library rules.
- [AppsFilterImpl](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/AppsFilterImpl.java): app-ID edge construction, permission-user→definer direction and trusted system-package force-queryable policy.
- [PackageManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/PackageManager.java): API contract and target-SDK compatibility boundary.
- [Framework resource configuration](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/res/res/values/config.xml): default system-package visibility configuration.

The fetched `ComputerEngine.java` SHA-256 is
`223614ff6d1694d0869b080c29fa869e1dd059b7abb8e0454fcb4d4b2b76c6dd`;
`AppsFilterBase.java`
`0b1b799409681fce4ec874efcaa278e9083896d3096310a23b666c2b7104b036`;
`AppsFilterUtils.java`
`10d1112948a187865ca7b740aead39dc266d52bf70cb64789257d9b3ecf737e1`;
`AppsFilterImpl.java`
`b4922b7c80454f2c4817499b73b7e5ddd419aea1093ed25c2aca9085f805185d`.

## Regression evidence and its boundaries

`tools/test_package_visibility.py` compiles the actual modified service, static
predicate and framework-backed fixture. Separate fresh VMs use the actual
protected and legacy runtime environment settings; the fixture does not replace
UID allocation with a fixed policy. The fixture is loaded separately from
production classes, matching the ART test/boot-loader access boundary.

The current host result is **58 assertions in protected mode and 22 in legacy
mode**. It exercises the real public `ApplicationPackageManager`, installed
snapshot/UID rules, shared-UID edges, all supported declaration families,
positive and negative filter cases, caller visibility, typed exceptions,
cross-user rejection, empty/null/duplicate requests, and removal/replacement.
The host adapters cover native identity/initialization and the unavailable
binary framework-resource catalog boundary. Unrelated native constructors are
omitted; the actual public manager reads a minimally initialized `ContextImpl`
user field. Parsed package records and shared UID/system-addon states are
constructed test inputs. These checks do not prove APK parsing, trusted addon
installation, ART or Binder transport.

Substituting the published `d4773067` package service into the same fixture is
the negative control: its first actual public self-query fails at the inherited
null-array result. The maintained runner accepts `--source-ref` for that control.

The service, helper and both affected public fixtures also compile against the
actual Android core module and framework with JDK 21, source/target 17. D8
conversion passed, producing a 77,396-byte focused DEX. Saved local evidence is
under `out/audit-package-visibility/`: `host-current.log`,
`host-published-negative.log`, `javac.log` and `d8.log`.

The new `PackageVisibilityContractTest` Windows entry point uses native runtime
initialization and writable framework cache memory, the actual framework APK,
generated Binder proxy/Stub/Parcel and the public package manager. Its manifest
inventory remains constructed, and it additionally checks actual framework
resource policy. The success marker is
`PackageVisibilityContractTest all passed:`. It must pass as a Windows gate;
host success is not substituted for that result.

`PackageQueryContractTest` independently extends its existing complete parsed
`components.apk` → real package service → generated Binder → actual system
context/public manager path with self/bulk query, typed missing-package,
null/empty/duplicate, user and removal/reinstallation assertions. Its existing
marker and all other package/cache/permission/property assertions remain.

## Explicit remaining limitations

This predicate has no retained or transient interaction visibility graph,
installer/initiator/update-owner records, overlay-actor mapping, instant-app or
SDK-sandbox visibility state, platform-signature inventory, or adb visibility
override. It does not infer those relationships from a package merely existing,
and does not grant every package visibility because some other package has
interacted with it. Runtime compatibility/device-config overrides beyond the
default target-SDK policy are also not implemented here.

Other package query and enumeration APIs retain their existing broader visibility
behavior; this change does not refactor all package APIs to use this predicate.
Consequently the result is the supported static `canPackageQuery` contract, not a
claim of complete Android package filtering. Multiuser installation state is
still absent. These are remaining compatibility gaps, not silently successful
implementations.
