# Provider authority lookup work

Related: #56 F13/T12 and #178 JAVA-06 (partial).

## Change and contract

`PackageManagerService.provider` and `ContentProviders.declaredProvider` previously
visited every installed package and generated every package's provider metadata for
each authority lookup. Hosted-provider conflict checks used the same full scan.

The new index holds all candidate package owners for each declared authority, in
installed order. It is built once for the current copy-on-write package-map identity
and is explicitly retired on install, replacement and removal publication. Keys are
bounded by installed first component declarations; misses do not add entries.

Every lookup still generates fresh `ProviderInfo` and `ApplicationInfo` objects and
applies current application/component enabled state, direct-boot/query flags and
pending removals. `ContentProviders` still examines all candidate owners and rejects
ambiguous authorities, including two different providers in the same package.
`PackageManagerService.provider` retains its existing first-result order. Provider
acquisition permissions and URI grants are unchanged. No permission or result cache
is introduced. The index and lookup use the package monitor, matching package and
enabled-batch publication; no provider callbacks or filesystem scans are added.

## Measured production work

Baseline: `751d82310f7c4c470c098ccebfd5d2d04699a0cd`. The fixture executes the complete
production PMS, ContentProviders and ApkPackage code against the baseline's built
Android 16 framework/core classes. Each constructed package owns one real parsed
provider. A `PackageImpl` subclass counts `getProviders()` calls while delegating the
actual metadata generation to Android. It changes no returned values.

Each warm case makes 32 calls. Hits target the last installed package. Android's
metadata generator reads the parsed provider list twice per generated package.

| Installed packages | Baseline declaration reads | Indexed declaration reads | Baseline PMS reads | Indexed PMS reads | Baseline miss reads | Indexed miss reads |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 64 | 64 | 64 | 64 | 64 | 0 |
| 50 | 3,200 | 64 | 3,200 | 64 | 3,200 | 0 |
| 500 | 32,000 | 64 | 32,000 | 64 | 32,000 | 0 |
| 1,000 | 64,000 | 64 | 64,000 | 64 | 64,000 | 0 |

This is **99.9% fewer parsed-provider reads for hits at 1,000 packages**, with no
provider metadata generation on warm misses. It is a workload count, not a Windows
latency, whole-process memory or whole-application speedup measurement.

The first declaration lookup builds the index. Its baseline/candidate read counts
for 1/50/500/1,000 packages are 2/3, 100/52, 1,000/502 and 2,000/1,002. A one-package
cold query therefore performs one additional metadata-list read. Index memory is
proportional to installed authorities and candidate ownership; metadata for all
providers within a matching package is still generated. Explicit component and
implicit-intent indices remain outside this change.

## Validation and reproduction

`tools/test_provider_authority_index.py` compiles production sources, prints their
SHA-256 values, and runs 32 contract assertions, 2,000 distinct miss probes and four
concurrent query threads across 300 package-map publications. It also reruns existing
canonical declaration tests. Coverage includes duplicate authorities/components,
aliases, installed order, disabled application/component and disabled-until-used
flags, direct boot, pending removal, replacement UID/authority changes, retirement,
reinstall, fresh mutable results and bounded immutable candidate lists.

```sh
python3 tools/test_provider_authority_index.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" --apksig "$APKSIG_JAR"
# The identical measured workload against the original production sources:
python3 tools/test_provider_authority_index.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" --apksig "$APKSIG_JAR" \
  --source-ref 751d82310f7c4c470c098ccebfd5d2d04699a0cd
```

The full app and service-test Java/DEX builds pass against the actual Android core
and framework classes. The existing URI-grant host regression suite passes 154
checks with the changed production resolver. Host JNI adapters supply framework
initialization and controlled Binder identity; these are not ART/Binder-transport,
APK installation, hardware or Android-reference runs. Fixture publications isolate
the in-memory package generation and do not exercise installer disk transactions.

The host fixture is in framework CI and its Android fixture is in the existing
Windows 2022/2025 system-contract track. Windows execution must be checked at the
submitted PR head. #249 also centralizes package publication for a permission
index; integrating both changes must retain both invalidations in `publishPackages`.
