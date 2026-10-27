# Shared Java libraries: bounded component evidence

This is component proof, not a complete CI result or merge acceptance.
Diagnostic **40524933356**, harness source
`1120e8a6c9303890bc5d267286e4cc29c3d24438`, attempt 1, is green on both
Windows versions. Both actual logs were read:

| Runner | Job | Actual result |
| --- | --- | --- |
| windows-2022 | 122261802698 | 21 scoped PASS, 15 real graph controls, 0 FAIL |
| windows-2025 | 122261801862 | 21 scoped PASS, 15 real graph controls, 0 FAIL |

Native runtime and supporting artifacts come from exact full CI
40512344714 / `96b12ab6f9b5146870c7a373e337f762010eb6f1`. Its supplying
jobs succeeded, but the full is red. Java comes from successful compiler
40523146557 / `f4378a98ccbd4d42496d44ba4dd3a7437c128ba0`; actual compiler
122255489323 reports javac exit 0, 0 errors and builds the real jar/APK
fixtures. The framework artifact is ID12743730526, digest
`sha256:a8e60d7025658430cb77f95d5bd542d3e9f2d9c66c6b6637acc61593121cbd13`.
The diagnostic validates both producers, source equality, immutable artifact
IDs/digests and repository/attempt identity before downloading inside CI.
It rebuilds all boot images from those exact Java jars with the original
profiles and speed-profile/speed filters; it uses no old image with new jars.

## Proven behavior

Actual ART validates the real XML registry, required/optional libraries,
dependency graph, SDK gates, cycles, canonical paths, content hashes and
artifact identity. Three separately compiled DEX jars provide an entry
library and two versions of its dependency. The APK calls them directly.

Both single/Main and device modes report separate app/entry/dependency
loaders and result1021000 with the original dependency. Independent oatdump
checks the nested PCL context, selected hot-method code and actual app image.
ART reports one image class, three methods and two methods with compiled code.

Updating the dependency at the same path without reinstall yields1037000
in both modes, AOT count zero and no old app image. The old oat bytes and
manifest remain unchanged. Installing the unchanged APK then recompiles,
publishes a different identity and executes the new result with compiled
code/image in both modes. A version2 APK requiring a missing library fails
before changing the old registry/APK; the old app still runs compiled in
both modes. Uninstall/shutdown, exact build.prop restoration and owned
system XML/jar removal all pass.

The single-process correction replaces GesherAppDriver's APK-only manual
PathClassLoader with LoadedApk's actual loader creation and profile
registration. It preserves the surrounding window/configuration/locale code.

## Retained failures and limits

- Full40503913306/c35 has two failures per runner from the missing
  UnixCopyFile.transfer native during fixture setup. That real API gap is
  owned by libcore-io; copying fixture bytes through streams does not close it.
- Full40512344714/96b has1 failure on2022 and4 on2025: the test73 active-log
  sharing failure on both, plus the recurring Web3 device A11y teardown and
  subsequent two missing NetActivity runs on2025. Both have all15 graph
  controls. Package23 and both ten-app e2e sets pass. No full-green claim.
- Diagnostic40520811180/d62 uses the shared-log correction and exposes four
  single-mode failures on each runner. Device checks pass, while Main's
  forced plain loader reports missing Entry/context expected1, found0.
  Compilerf437 and the fresh-image diagnostic above prove the bounded fix.
- The default after-app resource array is empty. Actual after-resource/DLC
  APK+image, APK static/SDK libraries, secondary dex and arbitrary factory
  loaders remain open. Serializer/unit checks alone do not establish them.
- A new complete coherent full, every original AOT/performance/Windows gate,
  actual package window/input/uninstall and both required e2e sets remain
  necessary before integration. The independent Web3 teardown recurrence
  requires investigation; its cause is not attributed to this loader fix.

All compilation and executable fixtures ran in CI. Local work was source,
hash/Git inspection and script parsing; no CI artifact was downloaded locally.
