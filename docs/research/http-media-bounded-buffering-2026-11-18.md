# Incremental HTTP media fallback — 2026-11-18

## Problem and change

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

`HttpSource` requests 256 KiB ranges. When the first request receives HTTP 200,
the old implementation downloads the entire response into one vector before
returning any bytes, then retains that vector for the source's lifetime. A large
media object therefore requires a correspondingly large private heap allocation.

The 200 fallback now keeps the single request and consumes it on demand through
one 64 KiB buffer. Successfully received bytes are stored in a file created
atomically with `CREATE_NEW`, no sharing or inheritance, and
`FILE_FLAG_DELETE_ON_CLOSE`. Backward seeks use the stored bytes; forward seeks
consume only the required prefix, with at most one transfer block of read-ahead.
No request is repeated to satisfy a whole-response seek.

This trades private payload memory for disk storage and disk I/O. The spool can
grow to the resource's full size. Windows may cache disk pages in reclaimable
system memory; the 64 KiB claim concerns the application's retained whole-body
payload buffer, not total process RSS or total machine RAM. There is no arbitrary
resource-size limit, silently truncated response, background download thread or
new transport backend. Disk failures return errors.

The existing immutable 16-entry range LRU and concurrent reader ownership are
retained. A malformed 206 body exceeding the requested 256 KiB chunk is rejected
before it can grow an unbounded range vector.

## Contract and lifetime

- Request headers, Windows proxy selection, certificate validation, the original
  URL and byte identities are unchanged.
- A valid Content-Length supplies the size without consuming the body. If a 200
  has no usable length, an explicit `Size()` drains the response into the bounded
  spool to obtain an exact size, preserving support for demuxers that need to
  inspect the tail. An ordinary early `ReadAt()` does not drain it.
- Premature EOF and transport failures remain errors at unavailable offsets.
  Already verified prefix bytes may be returned as a short read; no missing
  bytes are manufactured. Disk writes publish a new stored prefix only after
  the entire received block was successfully written.
- One mutex serializes the whole-response request, spool cursor and valid-byte
  count. Independent range requests retain their previous concurrency.
- Completion closes the request. Destruction and idle cancellation close the
  request and remove the spool. The request always retires before its parent
  connection and session.
- Cancellation is cooperative while a synchronous WinHTTP call is executing;
  the caller rejects its result and releases resources when that call returns.
  It does not close an in-use synchronous request from a second thread. This
  preserves WinHTTP's concurrency rules; immediate interruption of a stalled
  synchronous API remains outside this change.

Primary contract references:
[WinHTTP concurrency](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp),
[WinHttpReadData](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpreaddata),
[CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew),
and Gesher's internal `DataSource` contract in
[`data_source.h`](../../compat/media/player/data_source.h).

## Integration with extractor cache accounting

[PR #101](https://github.com/gesher-runtime/gesher/pull/101) has merged into main,
bringing `read_end_`/`CachedBytes()` accounting to `HttpSource`. The combined
implementation counts the committed disk-spooled prefix when a whole response
is active, and uses the range LRU only while range mode remains active. The
mode and valid prefix are published atomically; a cache query performs no network
or disk I/O, does not wait for a blocked spool transfer, and never counts a partial
failed disk write. Writers still serialize the spool cursor and append transaction.

Every positive public read advances the cache origin by the number of bytes
actually returned. This includes short successful prefixes before an error and
the same read that changes from ranges to a whole response. Failed reads do not
invent a new successful position, and cancellation reports an unavailable cache.
The host suite covers initial and unknown-length whole responses, seek, explicit
size discovery, failed transfers/writes, range gaps and both mode-transition
paths. The existing Windows loopback fixture checks the cache before releasing
the gated response tail, then after final/backward reads and cancellation.

## Reproducible comparison

Run from this branch:

```sh
python tools/test_media_http_contracts.py --measure --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
python tools/test_media_http_contracts.py --measure
ASAN_OPTIONS=detect_leaks=0 python tools/test_media_http_contracts.py --sanitize
```

The measurement compiles the complete production HttpSource with a declared
WinHTTP adapter generating an exact 134,217,741-byte resource. It uses actual
host file I/O behind the declared Win32 file boundary. Both versions receive the
same generated bytes and satisfy the same initial, tail and backward reads.
The adapter does not allocate a second full copy of the generated response.

| Measurement | Exact baseline | Updated |
|---|---:|---:|
| Transport bytes consumed before initial 32-byte read returns | 134,217,741 | 65,536 |
| Retained payload capacity after initial read | 134,217,741 | 65,536 |
| Retained payload capacity after reading the tail | 134,217,741 | 65,536 |
| Total resource bytes consumed after tail read | 134,217,741 | 134,217,741 |
| Whole-resource requests | 1 | 1 |
| Additional transfer for backward seek | 0 | 0 |

This is approximately 2,048 times less retained payload storage for the stated
128 MiB workload. One unsanitized host sample measured 221,164 microseconds to
the first useful read on the baseline and 759 microseconds after the change.
These timings include a synthetic in-process transport and host file creation;
they are illustrative, not a Windows/network/application speedup claim or a CI
timing threshold. The byte/capacity assertions are deterministic gates.

## Validation

The extended host gate exercises the actual production class with ASan/UBSan:
existing range behavior and LRU eviction, initial whole responses, zero reads,
known and unknown lengths, forward/backward seeking, guard bytes, short network
and disk I/O, empty responses, premature EOF, excess body data, transport errors,
spool create/seek/write/read failure, partial disk-write failure, unexpected disk
EOF, concurrent random readers, cancellation at blocked request-header and body
read boundaries, and idle cancellation. Every source must retire all owned
network/file handles, and every closed spool must disappear. The initial expanded
run passed 3,174 checks; concurrent range-cache activity can vary the number of
transport checks without changing the scenarios or expectations. The unchanged
expanded gate rejects the exact baseline with 25 failures, including the early
read and retained-buffer bounds; existing range/ownership assertions remain in
place. The separate measurement-only control succeeds on both revisions and
prints the comparison above.

The already-registered Windows fixture and its owned loopback peer add a gated
200 response: the peer withholds the tail until a separate request releases it.
The first read must therefore return before the remaining resource is available,
without relying on a performance timeout threshold. It also covers real
file-backed forward/backward/concurrent reads, unknown-length EOF, empty bodies,
cancellation and truncated transfers. Existing range checks are preserved.

Production source and the complete live-WinHTTP fixture compile and link for
Windows x64 and ARM64 using LLVM-MinGW `20261006`, C++20, `-Wall -Wextra -Werror`
and `-fno-exceptions`. Fresh Windows/ART execution, actual WinHTTP execution,
physical disk behavior on Windows, real proxies/TLS and end-to-end APK startup
remain unverified locally. Existing CI executes the host driver and Windows
fixture; no mandatory check or assertion was weakened. LeakSanitizer inspection
is disabled locally because the executor uses ptrace; ASan/UBSan remain fatal.
