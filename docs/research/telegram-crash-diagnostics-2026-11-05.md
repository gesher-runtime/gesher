# Telegram native crash: controlled evidence route

The original 02:30:56 Telegram crash still has no faulting PC. The previous
network success and debugger suspension recovery do not identify its cause.
This diagnostic step makes a fresh reproduction attributable without attaching
a debugger or suspending app threads.

`GESHER_TRACE_ELF_MAPPINGS=1`, set before starting the app process, enables a
fixed-format stderr record for each mapped native ELF object. Only the exact
value `1` enables it. Records contain PID, ABI, allocation base, ELF load bias,
mapped span and a bounded sanitized basename. They are flushed before TLS
rewriting, dependency resolution, IFUNCs and initializers can run. They do not
contain paths, app arguments or application payloads. A record means mapped,
not successful load completion.

The existing fatal patch provides RIP/RSP and PE module offsets. Subtracting
the matching ELF record's **load bias** from RIP gives the ELF virtual address
for disassembly. The allocation base/span are used to test containment; the
allocation base can differ from load bias when the first PT_LOAD starts above
zero. Mapping collection happens during normal loading. No loader lock is
acquired from a fault handler; calling `FindAddress` there could deadlock on
the interrupted loader thread.

For a per-app process log, extract the technical record only:

```powershell
python tools/extract_native_crash.py --log <per-process-log> --pid <app-pid> `
  --out out/telegram-network/current-crash-redacted.json
```

The extractor selects the last fatal record in that process log. It copies
only numeric signal/fault/register fields, sanitized mapping metadata and PE
module offsets. App messages, thread names, stack contents and fault messages
are excluded. A shared multi-process log must first be separated by process;
`--pid` selects ELF mappings and does not authenticate an unprefixed fatal
message's process. A missing fatal or unmapped PC remains explicit and is not
reported as an identified cause.

## Synthetic verification

The final diagnostic host SHA-256 is
`da36f059ff75a35fb42be9174bc974086d885a9a808f4f696f4917cb2bfef61a`.
All ten native installer outputs were built, including `profman`, `oatdump`,
`gesher-launch` and `gesher_shellext`.

Two private ART processes tested the fatal fixtures. With mapping enabled,
the ELF fixture faults at address 0x308 and resolves to
`fatal-jni-test.so+0x529a`. An independent `llvm-nm -S --defined-only` read of
that exact ELF places `Java_gesher_JniTest_fatalNullRead` at 0x5270 with size
0x2e, proving that the recorded PC falls inside the fault fixture. With tracing
disabled, the host fixture reports `ucrtbase.dll+0xed171` and emits no ELF
mapping records. Both intentionally exit 1 with SIGSEGV and valid RIP/RSP.
The Windows codegen track now requires this map-containment contract and the
disabled-trace control. Five extractor tests cover payload exclusion, load
bias, PID ambiguity, stale fatal records and invalid/out-of-range mappings.

Local synthetic results are in `out/telegram-network/trace-fatal-results.json`.
The artifact/source receipt is
`out/telegram-network/diagnostic-runtime-receipt.json`, SHA-256
`ff331571056aef01d432839052d062ee0c4c95282d575776c2fa4be3e7de8cf8`.
These results validate diagnostics; they do not establish a Telegram crash fix
or successful Telegram login. A fresh real fatal PC and matching native binary
are still required for causal disassembly and a targeted regression.
