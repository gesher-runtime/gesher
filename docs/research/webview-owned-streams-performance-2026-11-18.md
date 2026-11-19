# WebView native content ownership — 2026-11-18

## Baseline and scope

Exact baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

`Host::Navigate` and `Host::RespondToRequest` receive an owned native byte vector
from the JNI bridge. They previously initialized `SHCreateMemStream` from its
contents, keeping another full body in the memory stream. A copy of their queued
`std::function` also copied its captured vector. The ordinary production queue
currently moves these functions; the copied-function workload below is an
ownership stress case, not a claim that every existing dispatch copied twice.

The new path moves nonempty content into an immutable shared owner. Queued
callbacks retain this owner, and `OwnedStream` exposes the same bytes through
`IStream`. Each stream or clone owns its backing independently of the Host and
callback lifetime. The stream supports read, seek, stat and copy operations,
uses a separately synchronized cursor per clone, and rejects writes/resizing.
`IAgileObject` and an aggregated free-threaded marshaler permit background
reading without retaining apartment-bound application or browser interfaces.

An empty body is represented without a backing allocation. Ordinary GET
navigation and passthrough responses do not allocate a shared body. The existing
request guard, generation/deadline checks, fixed bridge headers, response
metadata, deferral completion and unavailable-host admission remain in place.
If stream allocation or marshaler creation fails, POST navigation reports the
failure without submitting an empty replacement body.

The Java response reader and the Java-to-native copy remain. Responses still
must be fully available before completion; this change does not implement
streaming or a total body-size limit. One native body remains alive while a
consumer needs it. A small shared-owner/stream/marshaler overhead replaces the
second payload-sized native allocation. This is not a guarantee that every
small response is faster.

## Deterministic measurements

The driver compiles the exact production Host methods and `Do`, the production
`Com` pointer template, and the complete `OwnedStream` header. Its explicit host
adapters replace the Win32 task queue, the WebView2 environment/request/response
objects, and COM marshaler construction. The baseline `SHCreateMemStream`
adapter copies the supplied initialization bytes, matching that API's ownership
boundary. It does not model Windows allocator or marshaler implementation costs.

The input vector is allocated before measurement. Each row dispatches one
4,194,304-byte body and then checks every returned byte. Payload allocations
are allocations of at least that size, inside native dispatch and queue handling.

| Workload | Payload allocations, baseline → new | Additional payload bytes, baseline → new |
| --- | ---: | ---: |
| POST, ordinary moved queue task | 1 → 0 | 4,194,304 → 0 |
| Response, ordinary moved queue task | 1 → 0 | 4,194,304 → 0 |
| POST, deliberately copied queue task | 2 → 0 | 8,388,608 → 0 |
| Response, deliberately copied queue task | 2 → 0 | 8,388,608 → 0 |

The same fixture counts two small dispatch allocations for empty ordinary GET
and one for a status-zero passthrough response, on both revisions. These gates
prevent replacing removed large copies with new allocations on those common
paths. Constant-size stream/COM/control-block allocations are not claimed to
be zero.

These are declared-boundary allocation and content measurements, not measured
Windows process RSS, Chromium memory, network throughput, APK startup time or
end-user latency. No end-to-end speedup is inferred from the removed copies.

## Validation

The host suite passes with AddressSanitizer and UndefinedBehaviorSanitizer.
It covers:

- Full/short/zero reads, EOF, unchanged output suffix, exact content and length.
- Unsigned absolute seeks, signed relative seeks, `INT64_MIN`, overflow and
  beyond-EOF positions; invalid pointer/flag arguments.
- Interface identity, aggregated-marshaler ownership, independent cloned
  cursors, backing retention and final release.
- Concurrent reads sharing one cursor, with each record delivered exactly once.
- Copy destinations that reenter the source, short/failing/invalid writes,
  aliased and optional output counters, and copying to the read-only source.
- Queued/copied/deferred navigation, exact POST metadata, content retained
  after Host release, stream/environment/navigation failures and rejected work.
- Response status/headers/content, stale and absent request IDs, all existing
  bridge admission guards, fixed bridge security headers, teardown and exact
  deferral completion.
- Allocation and marshaler failure without bodyless POST submission; the four
  payload-copy gates and the empty GET/passthrough allocation bounds.

The unchanged expanded suite rejects baseline dispatch with eight assertions:
four new payload-allocation budgets and four assertions against silently
submitting a bodyless POST when content-stream construction fails. The
measurement-only mode succeeds against both revisions. Helper-only tests always
exercise the new header; `--source-ref` selects the historical Host/Do methods,
not a historical implementation of the new helper.

The existing Java owned-response suite also passes using the prepared framework
classes. It exercises document publication, interception, concurrent ownership
and response bytes upstream of the unchanged JNI boundary.

The complete changed production `webview_host.cc` compiles for Windows x64 and
ARM64 with CI-pinned LLVM-MinGW `20261006`, the checksum-verified WebView2 SDK
`1.0.4258.31`, `-Wall -Wextra -Werror -Wno-unused-parameter -fno-exceptions`.
The new Windows COM fixture compiles and links for both targets. It marshals an
actual production stream from an STA to an MTA, releases the original owner
before unmarshaling, checks exact bytes and a retained clone, and verifies final
backing release. The fixture is built by the existing WebView workflow and is
required by the existing Windows WebView track and standalone Windows probes.

**Fresh Windows COM, actual WebView2/ART, browser network, and APK execution were
not run locally.** Cross-compilation and host adapters do not establish those
runtime results. Locally only LeakSanitizer process inspection is disabled
because it is unavailable in this ptrace environment; ASan/UBSan remain fatal.
CI keeps its ordinary sanitizer defaults.

## Reproduction

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_webview_owned_streams.py --sanitize
python3 tools/test_webview_owned_streams.py --measure
python3 tools/test_webview_owned_streams.py \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2 --measure
# Expected negative control: eight assertions fail on baseline dispatch.
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_webview_owned_streams.py --sanitize \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
```

For the native gate on a Windows machine, use the produced
`webview_owned_stream_test.exe`, or compile `tests/native/webview_owned_stream_test.cc`
with the pinned Windows compiler and link `ole32` and `uuid`. Its execution is
independent of an installed browser; it does not replace the existing WebView2
and APK tracks.

## Contract evidence

The requested Android operation and the application's actual data stay the
same. This changes private native content ownership, with no new capability,
permission, synthetic response or substituted application workflow.

- [Android WebResourceResponse](https://developer.android.com/reference/android/webkit/WebResourceResponse)
  defines the application-supplied response data and metadata.
- [WebView2 request content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourcerequest)
  and [response content](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourceresponse)
  require complete content at deferral completion and describe agile streams.
- [WebView2 threading](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model)
  documents background content reads.
- [SHCreateMemStream](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shcreatememstream)
  documents initialization from a byte buffer and allocation failure.
- [ISequentialStream::Read](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-isequentialstream-read),
  [IStream::Seek](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-istream-seek),
  [Clone](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-istream-clone),
  [CopyTo](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-istream-copyto)
  and [Stat](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-istream-stat)
  define the new stream's supported operations.
- [IAgileObject](https://learn.microsoft.com/en-us/windows/win32/api/objidlbase/nn-objidlbase-iagileobject)
  and [CoCreateFreeThreadedMarshaler](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreatefreethreadedmarshaler)
  define the cross-apartment mechanism and its restrictions.
