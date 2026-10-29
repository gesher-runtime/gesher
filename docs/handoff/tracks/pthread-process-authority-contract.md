# Process pthread identity and clock contract

ADR 0075 and roadmap leave non-current pthread CPU clocks open; the existing
current/process clock result does not cover them. Roadmap separately requires
the ELF loader and bionic shim for x86_64 and arm64 Android libraries. The
host CMake build explicitly links self-contained static executables. The
`gesher_bionic` static library supplies the ELF pthread and clock symbols with
the bionic/System V ABI; those guest identities are not the host winpthreads
ABI and require both guest ABI controls. Windows-host winpthreads static/DLL
consumers additionally require the same-process public behavior specified in
this work: a thread identity obtained from one participating module must name
the same live target when passed to the other, and a clock ID must resolve to
that target in both directions. A stale identity/clock cannot alias a later
thread after retirement or module unload. Errors, output preservation and
resource ownership must match the Linux references and existing controls.

Actual 409117 observed two independent host suppliers both issue pthread_t 1
to different live threads. Actual 409178/409191 show the clock-only executable
anchor can distinguish their clocks and survive DLL unload, but their
cross-provider pthread acquisition still picks each local record. The
same-thread dual-attachment behavior is under separate actual test. These
diagnostics do not qualify the public identity contract.

Candidate design under review: a versioned authority anchored for the host
process lifetime owns non-recycled public thread identities, records native
kernel-object capabilities and tracks each participating provider's live
registration. Each supplier must acquire or join the same canonical identity
before it exposes pthread_self or returns pthread_create. Registration,
acquisition and retirement must be atomic against one another; the authority
must never call back into a provider while holding its lock. Supplier-to-owner
lock ordering and rollback at each partial failure must be specified and tested.
Provider unload must not invalidate an active identity or cause stale reuse.
The same public identity is consumed by pthread_equal, clock acquisition and
the other supported pthread operations; fixing only clock_gettime is insufficient.
No implementation or missing-anchor policy is accepted by this document.

The current supplier lineage is not yet adopted: setup-build and its bypassing
arm64, webview and angle producer paths, exact static/DLL ABI, matching host and
translator, artifact source/role receipts, package notices and installation,
both guest ABIs and the original full Windows/e2e gates need independent proof.
The external-exit retirement supplier has separately bounded 409155 evidence;
combining its distinct patch lineage with this clock work requires fresh review
and current-source CI, not an inference from either component alone.
