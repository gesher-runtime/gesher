# Native loader integration source transition

PR107's classloader namespaces and actual library providers change four files
also covered by the OFD/stdio/pipe qualification. The integration joins feature
head `1084f49a538187c69534e63e2de63a2890f6515e` with main
`b88d0083d6ded06365f24181355516f2a1ecfffe`. The current-main composition preserves the separate
stdio metadata cache and the current OFD source qualification.

The original `ofd-stream-source.json`, its digest in `test-selected-policy.py`,
and `ofd-stream-source.patch` remain unchanged. The original selected supplier,
minimum supplier, socket ownership, descriptor, stdio, fixture, workflow and
approval checks still run. The current selected policy passes its 20 transition
mutation controls alongside the 31 native-loader controls.

## Exact admitted production changes

| Originally protected file | Native-loader change | Main behavior retained |
| --- | --- | --- |
| `compat/bionic/stdio_bionic.cc` | Split the existing liblog registrations into `RegisterLog`, so libc and liblog have their actual owners. | All formatter, stream, close/reopen, locking and OFD implementation bytes. |
| `compat/bionic/extra_bionic.cc` | Separate libc, libm, libdl and private linker registrations; replace aggregate platform handles and global search-path state with the actual ELF namespace/provider implementation. | Actual stdio, properties, math and OS adapters; no replacement provider implementation. |
| `compat/bionic/shim.cc` | Register actual provider tables, keep internal-only names separate, and dispatch guest libdl lookups through caller/namespace/version context. | Current main's OS adapters, signal and memory behavior, and the shared environment owner. |
| `compat/elf/exec/gesher_exec.cc` | Register the existing executable entry/exit/getauxval/program-name overrides with their libc owner. | Main's process initialization, descriptor transport and shared `EnvironmentSlot`; the obsolete independent `g_environ` overrides are not restored. |

The same patch contains the five directly coupled changed headers:
`extra_bionic.h`, `bionic_shim.h`, `shim_internal.h`, `elf_loader.h` and
`elf_dlopen.h`, with their full paths recorded in the manifest. The unchanged
`compat/arm64/include/gesher/symbol_table.h` is compared directly with main.
No other protected file is normalized.

These are production changes that require independent scope and correctness
review. A successful byte round trip proves the boundary of the admitted
delta; it does not establish Android compatibility or grant merge approval.
The review must inspect the actual provider ownership, namespace/ABI behavior,
main composition and the new qualification code, rather than treating this
document or passing tests as approval.

## Verification boundary

`preceding_native_loader_sources` in `test-selected-policy.py` checks:

1. A separately pinned manifest, fixed baseline and feature identities, fixed
   source/header path lists, and the original main policy's digest.
2. The patch digest and exact nine patch paths, then every current endpoint's
   full digest and every baseline endpoint against the named Git commit.
3. The four protected baseline digests against the **original** OFD receipt.
4. Exact reverse application to all nine main files, followed by exact forward
   application to the current files. A new full-file digest alone cannot admit
   an unrelated source or header change.
5. The unchanged shared symbol-table header against main's actual bytes.

Only the four restored source values are supplied to the original 71-entry OFD
digest loop. The five POSIX files in the original OFD patch are not modified by
the native-loader normalization. Their existing reverse transition, protected
function comparisons and earlier TCP/supplier assertions run as before.

The patch contains only changed lines (zero context); the full-file endpoint
digests and exact reverse/forward comparisons also bind every unchanged byte.
It is an explicit fixed main-to-feature delta, not a path exemption,
function-sized exclusion, generated approval receipt, moving branch reference,
or mechanism for accepting arbitrary new hashes. Changes to either endpoint
require a new production and qualification review.

## Composition with main's memmem transition

Main `324dc7e41ea13ceedc3d52db3c8f7c2f3590e452` includes PR274's independently
pinned `memmem` helper and two exact wrapper/include substitutions. The current
shim contains both changes. Before checking the frozen namespace endpoint, the
policy reverses the exact memmem substitutions on both endpoints and validates
the unchanged helper digest. Both shim values therefore compare against the
same independently pinned OFD bytes. The current-main namespace manifest and
patch are updated for the new baseline while the current OFD/supplier receipts
remain intact.

After the namespace reverse/forward round trip, the policy reapplies both exact
memmem substitutions and requires equality with the actual combined shim. A
missing transition, unrelated provider change or extra source edit cannot be
hidden by changing an endpoint hash. The original main memmem controls run
alongside the namespace controls; composition does not grant an exemption to
either feature.

## Reproduction and negative controls

From a checkout with the pinned history available:

```sh
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-native-loader-policy.py
python3 tools/net-tcp-priority/test-vector-policy.py
python3 tools/net-tcp-priority/test-selected-supplier.py
python3 tools/host-pthread-authority/minimum_receipt.py selftest
```

The native-loader policy test mutates only in-memory inputs. Its 31 controls
cover all nine admitted files, source/header edits despite an updated endpoint
digest, an undeclared include, an unchanged coupled header, expanded file and
header lists, an undeclared patch path, a modified before-hunk despite updated
patch and manifest digests, changed baseline/feature/policy pins, four unrelated
protected files and the original OFD receipt. The composition controls also
reject either transition being missing and an aggregate-symbol fallback despite
a repinned normalized endpoint. Existing descriptor, socket,
stdio, vector and supplier negative controls remain separate and unchanged.

The associated behavior gates include production ELF namespace/identity,
symbol-version, atomic-load/rollback, packed-relocation and lookup contracts;
actual Bionic/graphics registration against the pinned AOSP export maps;
shared environment and stdio ownership contracts; real JVM classloader/native
dependency policy; and actual Windows target compilation and linking. Host
fixtures describe their Win32/JVM/translator boundaries explicitly. Cross
compilation does not establish Windows execution, ART lifecycle or UI behavior;
those remain the existing Windows CI gates.
