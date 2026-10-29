# Whole provider graph and remaining ABI routes

The initial/stress executable is not whole-host qualification. The production
dependency graph is `.github/workflows/ci.yml`, not every historical scoped
workflow in the repository. Its reusable producers are framework, native-libs,
exec, libs, arm64, native2, posix, graphics, angle, webview, boot-image, media,
surfaces and ordinary Binder guest fixtures. Host executable producers using
`.github/actions/setup-build` now build the private pinned authority supplier;
each must ship its actual supplier receipt and bind its linker inputs to it.

Three production producers bypass that action: arm64, webview and angle.
They independently select the cached original compiler. Inspect their actual
link commands and libraries, stage a separate verified patched supplier when
winpthread is linked, record a reviewed no-winpthread role when it is not,
and rebuild their outputs. ARM64 must never be swapped alone into an older
host package. Android NDK guest libraries are a different ABI/provider and
retain their existing source/compiler/archive identity controls.

The common action currently downloads and verifies a complete private compiler
archive before whole supplier replacement. Its old shared compiler cache is
preserved. Record both wrapper and underlying Clang binary identities, exact
configuration/build/link commands and static archive/DLL/import/header/notice
digests. Add the supplier notices and modification provenance to installed
package contents and verify them through package install/run/uninstall. Every
consumer artifact must retain exact source/run/attempt/role and strict ZIP
producer receipts. Do not mix current host outputs with previously cached
translator/package roles that lack this provider identity.

The host `time.h` overlay reaches rebuilt source callers including ART that
include that header. A prebuilt library or a caller resolving the literal
`clock_gettime` symbol bypasses it. This is an explicit unclosed ABI route.
Choose and review either provider-owned core dynamic-clock dispatch, or a
link-level core wrapper that covers every supported caller. Prove the direct
function-pointer/symbol route and a separately compiled prebuilt consumer,
using genuine target clocks and independently retained Win32 oracles. Header
macros by themselves do not close this route. Keep fixed process/current
clock dispatch and errno/output-preservation controls unchanged.

Production test91 is reserved under ADR0075. Add it to both canonical Windows
runners only after independent source and component review. The full must
rebuild all changed consumers and preserve test90/both guest ABIs, actual
ART/APK, original suite, installed package23 and both complete e2e gates.
Any code combination differing from qualified sources needs a new full.

Fault controls belong to a separately named supplier and host build. The
production supplier/object/DLL must prove test-control symbols absent and
compile definitions OFF. A separate supplier patch may expose deterministic
duplicate failure and irreversible generation exhaustion; a host-only test
compile definition may expose irreversible clock-ID exhaustion. Both use
genuine live targets, unchanged failure outputs, cache success at exhaustion,
and final real target exit/handle reclamation. No production runtime
environment variable selects or enables a fault path.

Actual408038 attribution: fixture-owned handles are zero at retirement;
257-target capacity returns to its pre-stage process handle count; each
detached race adds one process handle and1024 rapid detached exits add1024.
Determine actual retained object type and identity before any supplier fix;
preserve the original baseline+8 bound and exact failing runs.
