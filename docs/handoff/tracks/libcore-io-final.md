# Libcore UnixCopyFile registration evidence

Full40550474909, attempt1, exact source
`944a0eeffe5157d662f0278e992c421eebc86255`, is green28jobs22success/6skip.
The integration worker independently read all job conclusions and the five
actual critical logs, saved under C:/work/gesher-ci-review/libcore-io-40550474909-*.log.
No CI artifact was downloaded locally; all compilation/execution occurred in CI.

| Actual job | ID | Required evidence |
| --- | --- | --- |
| Windows2022 |122357220238|All15 genuine CopyFileTest groups, final verdict, whole0 |
| Windows2025 |122357220256|Same15 genuine groups, final verdict, whole0 |
| Package |122357220244|23 actual installed image/font/app/window/input/resize/uninstall checks |
| E2E2022 |122358441959|Ten apps,4 transport controls, association/shortcut,0 unexpected |
| E2E2025 |122358441807|Same complete set,0 unexpected |

Each actual child prints six exact-byte copy groups for0,1,8191,8192,8193
and2097223 bytes, existing-destination preservation, explicit replacement,
missing source without partial destination, real descriptor offsets/ownership,
real allocated cancellation word checked after read/before write, sourceEBADF
before cancellation, EOF before cancellation/destination access, destination
EBADF after actual read, and successful full transfer after clearing the same
cancellation word. Both then print `CopyFileTest all passed`; each enclosing
Windows suite ends `0 test(s) failed`. These are15 child groups plus one
separate aggregate shell verdict, not16 distinct native groups.

The production change is the exact existing JNICALL declaration/table entry
for the already-linked pinned upstream UnixCopyFile.transfer body. No copy
replacement, fabricated cancellation or production IO semantic change exists.
Logging-only4671d3dd exposes successful actual child output while preserving
every expectation and exit gate; the shared process helper is unchanged.
Full source includes accepted resolver integration3fb8255d. The source review
and pinned native/Java/source-list hashes are retained in ADR0077.

Prior full40522376074/2913d484 remains successful but suppressed those child
records; it was not independently accepted as all15 visible-group proof.
The new full resolves that evidence limitation. Head drift after944 is docs
only; integration must preserve exact non-doc equality or run a changed union.

This accepts registration and the15 observed groups only. Induced short-write/
EINTR, concurrent asynchronous cancellation, filesystem metadata/symlink
semantics and other libcore name-found natives remain explicit open scope.
No broad libcore/IO or release-completion claim follows from this leaf.
