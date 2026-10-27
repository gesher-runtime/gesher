# Native allocator accounting observation

CI-only research for native3's open ART Heap::GetNativeBytes contract. ART
still substitutes a constant malloc byte count on Windows. This probe does
not change production accounting or close a roadmap box.

It resolves [HeapSummary](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapsummary)
at runtime, queries the actual [_get_heap_handle](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-heap-handle)
and records allocation/reallocation/free deltas and query cost. A separate
VirtualAlloc mapping checks that native heap statistics do not count the
managed heap's mapping mechanism. Independent locked HeapWalk totals are
printed for comparison; exact summary/walk equality is not assumed.

Microsoft documents HeapSummary/HEAP_SUMMARY from Windows build 20348;
the installer currently admits Windows 10 generally. Availability/failure
handling and older-Windows support must therefore be designed before a
production fix. A process working-set/private-byte replacement would count
mapped Java heaps, and hot-path heap walking would require explicit latency
and locking review. See [HeapWalk's documented locking/cost](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapwalk).

Both Windows runner logs must be inspected. Every compile and native run
occurs in native3-probes CI; no local native/Java build or execution.
