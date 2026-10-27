# Net4 legacy host/service resolver evidence

[Full40519019567](https://github.com/gesher-runtime/gesher/actions/runs/40519019567),
attempt1, exact source `f36317637e01fdeeebcb1cbc4777c1cc8f9cea9a`, is green:
28jobs22success/6skip. Both actual Windows logs contain730 top PASS records,
no top FAIL and `0 test(s) failed`. The source includes accepted runtime
bc6e05be and integration3577b330's documentation. All22 successful job logs
and the complete28-job metadata were examined; no artifact was downloaded
locally. Only compilation and actual execution in CI supply runtime evidence.

| Actual job | ID | Evidence |
| --- | --- | --- |
| Windows2022 |122259610612|18 test82 PASS,16 real NDK caller contexts,0 failed |
| Windows2025 |122259610623|Same complete set,0 failed |
| Package |122259610574|23 installed runtime/window/input/uninstall PASS |
| E2E2022 |122260927674|All ten apps,4 transport controls, association/shortcut,0 unexpected |
| E2E2025 |122260927652|Same complete set,0 unexpected |

The existing gethostbyname/gethostbyaddr/getservbyname/getservbyport exports
now publish deeply owned Linux-layout results rather than raw Windows
hostent/servent storage. Host family/length are full32-bit fields and service
protocol/port order matches LP64 bionic. The actual three production units
also compile with llvm-mingw/Werror/exception-free flags in scoped40517730958;
the dedicated NDK/JNI/ELF/APK builds passed40516203951 and this full build.

For EACH Windows runner, each ABI runs allow/deny standalone ELF, direct ART
JNI, single APK and device APK:16 separate contexts. Allowed standalone
callers each print `allowed=1; checks=31; oracle=8`. The other allowed callers
each print22 checks; all denied callers print20 checks. Every context runs
four real concurrent native threads and requires distinct/stable host,
service and h_errno storage while other threads resolve/fail. Mutable copies
cannot corrupt the provider; service calls preserve held host/error state.

The separate WinSDK process decodes Windows' actual short fields and compares
eight host/service records with each standalone ABI. Actual IPv4 family2/
length4 and IPv6 family10/length16 agree; addresses are127.0.0.1 and::1.
HTTP/TCP aliases are www,www-http; HTTPS/TCP port is47873 in network byte
order, not sign-extended. Domain TCP and UDP use13568. Canonical localhost/
reverse names match each runner's actual host, rather than a hardcoded name.

Meaningful negatives retain exact outcomes: invalid reverse family is
NETDB_INTERNAL/EAFNOSUPPORT; wrong length is NETDB_INTERNAL/EINVAL; strict
numeric shorthand/invalid IPv4 and scoped local IPv6 are rejected; absent
INTERNET denies nonnumeric/reverse calls but permits numeric conversion and
local service lookup. Missing service/protocol and out-of-range ports yield
no fabricated object. Both manifest variants, both ABIs and both app modes
require actual native verdict, fresh normal child exit and cleanup.

Regressions retain38 top Windows4,14 Web3, four exit70, ten format71 and two
action74 PASS per runner; Web3 has five VM0, six launcher0 and six fresh
child0 records. Ifaddrs standalone callers still match nine actual host
records; all original Native2 modules/app/manifest remain unchanged.
Strict AAudio750/1250Hz/44100-frame/capacity/zero-loss controls pass.
Actual installer rendering/input/resize, uninstall data/process/association/
shortcut cleanup and both e2e sets including Just Player/Puzzles/TIC80 pass.

This fixes four pre-existing exports, not four new entries from the84-entry
historical missing-symbol inventory. The inventory JSON and its145 existing
proof objects remain byte-identical;82 remaining Net4 inventory entries stay
open. Provider-specific TRY_AGAIN/NO_RECOVERY and heap-exhaustion induction,
reentrant/name2/enumeration/wire/query/per-network policy, other socket and
hardware contracts remain open. Only Root can independently accept/merge.
