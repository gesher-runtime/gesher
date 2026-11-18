# Native Windows contract observations

This temporary diagnostic branch investigates the unresolved design boundaries
in PR73 and PR100. It does not change or link Gesher's production code. A green
workflow means only that observations were collected successfully; individual
rows can disprove a candidate design. No row approves Android compatibility.

The pipe probe tries both original endpoints and all three requested access
modes using `ReOpenFile` and `NtOpenFile` with an empty name relative to the
captured endpoint. It records independent wait-mode behavior and actual reads
and writes against the original queue. It covers both an inbound byte server
and PR73's exact native topology: a duplex message pipe with directional
published duplicates and both full-access creator handles closed. A successfully opened handle alone is
not evidence of a correct independent pipe description. This does not test
cross-process inheritance, cancellation or concurrent native consumers.

The directory probe holds a real writer open while replaying three problematic
rename/write histories and a no-rename control. It starts the first extended
directory read before the operations and deliberately defers collection until
after them, exercising buffered notifications between reads. It records native
write-only behavior, explicit current write-time updates, and an explicitly
unchanged-timestamp negative control. The timestamp modes are experiments in
isolated temporary files; no timestamp update is adopted by production code.
The preparation writer is closed before the watch is installed; the history
uses a new, still-open writer so deferred preparation metadata is excluded.

The first diagnostic run, 44256210705, failed setup for the inbound-pipe control
on both Windows images. Its PR73 topology rows remain valid negative reopen
observations. This revision selects the inbound server's nonblocking mode at
creation and reports the exact failing setup stage. It also excludes the
preallocation metadata that contaminated the first directory-history controls.
The original failure logs are retained; neither revision is production approval.

Raw `FILE_ACTION_MODIFIED` order is printed separately from the extended
metadata. Its interpretation as Android `IN_MODIFY` versus `IN_ATTRIB` remains
unqualified; matching the raw sequence cannot establish that distinction,
permissions, lossless generation correlation or behavior on other filesystems.
No result from a 200 ms quiet interval is a universal ordering proof.

Each fixture creates only its own process- and time-qualified temporary directory
and named pipe, closes owned handles and deletes its three possible file names.
The workflow kills an overlong owned process after 60 seconds and retains the
logs. It runs with repository read permission only, without persisted checkout
credentials, on Windows 2022 and Windows 2025 using their installed MSVC.

Primary design references:

- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfiletime
- https://learn.microsoft.com/en-us/windows/win32/sysinfo/file-times

The existing PR73 and PR100 negative evidence remains unchanged. These
observations must be evaluated alongside the actual production implementation
and its independent Android/Linux contract checks before designing a repair.
