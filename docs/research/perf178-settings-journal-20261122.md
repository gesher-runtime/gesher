# JAVA-09: durable settings mutations without full-store rewrites

This is a focused implementation for [performance epic #178, JAVA-09](https://github.com/gesher-runtime/gesher/issues/178).
The baseline is `8b316aebf09ab46d5407e955bf86ed748a1e739f`.
It does not claim completion of the epic's Windows workload, full-process memory,
crash/power-loss or eight-hour qualification criteria.

## Production change and contract

`SettingsStore.put` and `remove` previously serialized every row in all three
tables, synced a temporary file and replaced the store for every actual mutation.
The provider already serializes accesses under its table lock. The new implementation
keeps that lock, the API, per-change durability, row-generation semantics, null and
binary UTF-16 encoding, no-op detection and existing provider authorization unchanged.

The store now contains one checksummed checkpoint and a bounded suffix of row
mutations. A normal change appends one length-delimited, checksummed record and calls
`FileDescriptor.sync()` before returning success. It does not defer, debounce or
combine successful writes. A checkpoint replaces the file after 128 journal records,
or before adding a record that would exceed a 1 MiB suffix. A newly added default also
forces the next mutation to checkpoint, preserving the existing `addDefault` behavior.

The single-file format avoids a separate snapshot/journal generation protocol:
publishing a checkpoint atomically replaces both old pieces together. Publication
retains the existing temporary-file sync and `ATOMIC_MOVE`/replacement fallback.
There is no new background thread, timer, executor or retained file descriptor.

### Format and recovery

GST1 files remain readable without being rewritten on load. The first actual change
migrates to GST2 using checkpoint publication. GST2 contains:

1. Magic, next row ID, and the existing three-table snapshot layout.
2. CRC32 of the complete checkpoint before its checksum field.
3. Zero or more records: payload length and its bitwise complement, next row ID, table index, operation,
   UTF-16 name/value as needed, and payload CRC32.

Replay validates the length/complement header before accepting an incomplete payload
as a torn write. This prevents a corrupted length from hiding an acknowledged row.
It also validates table/operation, framing, key/value limits, checksums and generation
progression before publishing each row to the temporary load state. A final incomplete
record is excluded; the acknowledged prefix is restored, and the incomplete tail is
truncated and synced before another append. Complete corrupt records/checkpoints
raise an explicit storage failure instead of silently replacing acknowledged rows
with defaults. Legacy GST1 malformed-input behavior remains unchanged.

Failed appends truncate to the previous complete length and sync that rollback before
the in-memory mutation is rolled back. If rollback itself fails, the writer refuses
later mutations until the store is reopened and recovered. A close failure after a
successful sync does not turn an already committed mutation into a reported failure.
Failed checkpoint writes, syncs or replacements preserve the old store and row IDs.

The append serialization buffer is bounded by one allowed row, independent of total
table size; replay reads at most one bounded record at a time. The journal is capped by
both bytes and record count. A periodic checkpoint still performs work proportional
to all rows, and the provider continues to hold its table lock while syncing. This
change intentionally does not claim lock-free reads or eliminate sync latency.

The private persistent format changes. Older runtimes that only understand GST1 cannot
read a GST2 store; backwards migration/downgrade is not implemented. No application-facing
setting names, values, permissions, notifications or return semantics are changed.

## Validation

The existing `tools/test_java_performance_contracts.py` CI invocation runs the expanded
production-source test. The settings component also runs in the existing
`windows-host-contracts` matrix on Windows 2022 and 2025 to exercise native host
replacement, sync, truncation and process-crash recovery. Settings tests compile the actual
production class, replacing only stream construction, sync and rename boundaries with
counting/fault adapters that delegate to real host filesystem operations.

The settings fixture passes **516 checks**, including:

- Exact legacy GST1 fixture import and migration; all three tables, IDs, deletion,
  null/empty/binary UTF-16 values, maximum key/value size and forced same-value generations.
- Count and byte checkpoint limits across reopen cycles; new defaults; unchanged-value
  writes; replay after updates and tombstones.
- Every byte truncation point of a new append, including partial headers, with a subsequent
  successful mutation and reopen; every header/payload/checksum byte corrupted independently;
  an enlarged in-range length regression; rejection without modifying corrupt storage;
  and checkpoint checksum corruption.
- Injected partial writes, append sync failures, checkpoint sync/replace failures,
  post-sync close failure, failed rollback and refusal to continue an uncertain writer.
- Eight concurrent callers serialized using the provider's existing lock contract.
- Abrupt child-process `Runtime.halt` during an append and during an unpublished checkpoint;
  and immediate halt after an acknowledged mutation, with parent-process reopen checks.

The complete existing Java performance suite also passes: 28 service Proxy dispatch
checks and eight APK discovery checks. Its existing allocation/discovery gates remain.
These are host tests with real files, not Windows/ART system-service execution.

Reproduce:

```sh
python3 tools/test_java_performance_contracts.py --java-home "$JAVA_HOME"
python3 tools/test_java_performance_contracts.py --java-home "$JAVA_HOME" \
  --component settings --settings-benchmark
python3 tools/test_java_performance_contracts.py --java-home "$JAVA_HOME" \
  --component settings --settings-benchmark \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f
```

## Measured host workload

[Raw results](perf178-settings-journal-20261122.json) record the baseline commit and
candidate production-source SHA-256. Five alternating A/B pairs use a fresh OpenJDK
21.0.13 JVM on Linux x86_64 for each sample. The synthetic fixture creates 5,000 settings
with 96-character values, publishes an initial checkpoint, then changes one row 256
times. Every result is reopened and compared with the live table. Both implementations
use real file I/O and retain all 256 sync operations.

| Metric for 256 mutations | Baseline | Candidate |
|---|---:|---:|
| Bytes passed to file writes | 297,688,100 | 1,176,399 |
| File-write boundary calls | 36,352 | 397 |
| Successful mutation syncs | 256 | 256 |
| File replacements | 256 | 1 |
| Median elapsed time | 1,781.43 ms | 24.93 ms |
| Median calling-thread CPU | 1,778.96 ms | 24.74 ms |
| Median calling-thread allocated bytes | 2,813,536 | 184,288 |

Write volume falls **99.60%** in this fixture because only one periodic checkpoint
rewrites the table. This is a deterministic I/O-volume comparison, not a claim about
physical disk wear or a whole-application speedup. Host CPU and elapsed samples are
informational: JIT warmup, filesystem caching and host scheduling affect their values.
Calling-thread allocations are not process private commit, resident memory or peak RAM.

Remaining qualification includes Windows/ART provider execution, representative setting
counts and mutation patterns, physical-storage/power-loss behavior, whole-process
memory, multi-application workloads and integrated soak testing. Existing atomic-replace
fallback semantics are retained; these host checks do not establish filesystem durability
after a machine power failure.
