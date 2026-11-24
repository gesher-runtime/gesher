# 0091: Which APKs of a bundle an install takes

Status: proposed, 2026-11-24 (see docs/installer.md, "The Gesher device and
(un)installing")

## Context

Apps reach Gesher as single APKs and as ZIP archives of APKs: bundletool APK
sets (`.apks`, `bundletool build-apks`), APKPure's XAPK, APKMirror's APKM and
APK+. Android has no install path for an archive. `PackageInstaller` sessions
and `adb install-multiple` take a set of APKs that is already one install: a
base APK and the splits the device needs. Something else chooses that set: Google
Play from the app bundle, `bundletool install-apks` from an APK set's `toc.pb`,
or the store's installer from its archive.

An APK set is not one install. It holds a standalone APK per ABI for SDK
levels below 21, a split variant per SDK range, instant APKs, every device's
ABI, density and language splits, on-demand modules and asset packs. Taken
whole it has several base APKs. An XAPK or APKM usually holds one install, but
may hold more splits than a device needs (APKM holds every density and ABI), and
an XAPK lists its APKs in `manifest.json`.

## Decision

1. **Content decides the format.** An APK has `AndroidManifest.xml` at its ZIP
   root; a bundle has none there and holds `.apk` entries. The file name only
   picks the error message for an encrypted `.apkm`.
2. **An APK set installs what bundletool would install on Gesher's device.**
   `com.gesher.app.ApkSetToc` reads `toc.pb` and follows bundletool's
   `ApkMatcher` and `VariantMatcher`, a matcher per targeting dimension, with
   Gesher's device: its SDK level, its ABIs in order (`NativeLibs.SUPPORTED`:
   x86_64, then arm64-v8a), no SDK runtime and no texture compression formats.
   Gesher's device defines no device tier, country set or device group, so, as
   `bundletool extract-apks` and `install-apks` fill a device spec without them
   (`ExtractApksCommand.applyDefaultsToDeviceSpec`), it takes the set's own
   defaults from `toc.pb`: its default device tier or tier 0, its default country
   set or none (the fallback splits), its default device group or none. Device
   features and OpenGL versions, which Gesher's spec does not list, do not
   exclude a conditional module, as in bundletool for such a spec; this is a
   known limitation, since Play would leave out a module whose required feature
   the PC lacks. Decoding `toc.pb` is bounded by a number of fields, so a small,
   highly compressible archive cannot exhaust the installer's memory.
3. **Every density and language split of the chosen modules is installed.**
   The matcher implements both dimensions, but Gesher's device leaves them out:
   its density is set per launch (`GESHER_DISPLAY`), its languages follow
   Windows' settings and per-app locales at any time, and no store delivers a
   missing split later as Play does. Installing them all is a set Android's
   package manager accepts (`adb install-multiple` with every split), and
   resources choose among them at run time.
4. **An XAPK's `manifest.json` names its APKs**; other archives without `toc.pb`
   install every `.apk` they hold.
5. **Installed files are named from their manifests**, `base.apk` and
   `split_<split name>.apk`, as `PackageInstallerSession` names them, whatever
   the archive called them.
6. **What is not an APK is reported, not invented.** Expansion files are logged
   as not installed; an encrypted APKM is refused with a message saying so.
   A `manifest.json` that starts with a UTF-8 byte order mark is read as the
   JSON after it.

## Consequences

- An `.apks` from `bundletool build-apks` (default or universal mode) installs.
  Its standalone APKs install only when no split variant fits the SDK level.
- An app with asset packs targeted at device tiers, country sets or device
  groups gets the slices for the set's defaults, as `install-apks` gives them
  without a device spec naming them.
- An app that needs a density or language split installed later, as Play
  would add it, already has it.
- Disk use is that of all density and language splits of the chosen modules.
- An app that needs its OBB files does not find them in `Android/obb/<package>/`
  until something places them there; doing so belongs to Gesher's
  shared-storage ownership, not to the package install.
- Bundles installed before this change by other file names get the manifest
  names on their next install, which then counts as an update.

## Alternatives considered

- **Every APK of an APK set** (what Gesher did): fails on several base APKs.
- **Choosing by file layout without `toc.pb`** (`splits/`, `standalones/`,
  `universal.apk`): cannot tell two split variants' files apart
  (`base-master.apk`, `base-master_2.apk`) or which modules are install-time.
- **Only the device's density and language splits**, as `install-apks` does:
  the app would lose its strings or best drawables after a change of display or
  language, with no way to fetch them.
