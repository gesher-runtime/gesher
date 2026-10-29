# TSF and Services controller: bounded console and census work

This is CI-only supervision of the fixed trusted TSF and Services probes on a
trusted runner. The inherited console is borrowed, not an owned or authenticated
console host. Console membership proves attachment, not authority or exclusive
access. Standard handles are explicitly whitelisted; output files allow sharing
during execution and the final exclusive read proves retirement of open writers,
not protection against a hostile same-user writer that opened and closed a file
earlier. This assumption does not apply to Binder or guest trust.

Run 40869317576/source 9c7de861 was red on both Windows controllers: after an
exited retained child passed its parent query, a second PID from the older job
list could not be opened (Win32 87). Unknown unretained identities must never be
authorized by recensus. Raw logs are in `C:/work/gesher-ci-review/tsf-408693-controller-{2022,2025}.log`.

Run 40869961275/source 57283386 passed both controlled and actual TSF jobs: the
retained-child exit and Toolhelp absence, denied parent query, wrong PID, unknown
child, orphan, retirement and output controls passed; both actual TSF probes had
zero failures. This is bounded parent-query proof, not combined full acceptance.

Run 40871088307/source cba69331 passed both TSF jobs with a one-process limit:
the same child ran in the unrestricted positive control; the restricted attempt
failed with Win32 1816, no child marker, total one, and clean root retirement.
The later Services run 40871529177 was red: detached PowerShell exited zero
without executing its script. Runs 408728 and 408763 distinguish attachment and
console creation; the runner has a console even though its HWND is zero. Run
40877220121 proved the inherited-console diagnostic script and both streams on
both Windows with total one, but actual Services remained red. These diagnostic
runs do not qualify the current candidate or a combined full.

Current source must separately prove real changed-console membership, missing
and unavailable-list controls, actual TSF and the Services worker, compiler and
provider on both Windows. The active-process limit applies to simultaneous job
members; the cumulative total-one check is independently required. Any vanished
unretained identity or unproved cleanup remains a failure, not a trusted host.
