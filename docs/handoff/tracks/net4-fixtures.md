# Net4: deterministic HTTP and certificate fixtures

Source slice imported from Media3 836482975254ec3fbcbbdb190398d65dc79fae11.
The combined patch has SHA-256
73d3132e04c05833c396cf095e3529a251f1b1ececa2d0cb0f7de01f886a2c3b
and changes five test files only. Root independently reviewed the Java,
PowerShell and C# source, verified the patch hash/application and parsed
both PowerShell files. No compiler, certificate generator, TLS fixture or
JVM ran locally. The coherent Media3 full run will supply fresh runtime
proof; no duplicate Net4 full run or acceptance is claimed now.

## Preserved behavior

Actual example/Google DNS, Google HTTPS204 and Google TLS1.3/SNI/hostname
checks remain unchanged. Four distinct local certificate failures retain
real certificate/hostname validation and never accept a connection timeout
or unrelated handshake error. A valid ephemeral test root enables local
HTTP200/body and NetApp HTTPS in single-process/device modes.

Two distinct SAN certificates share one virtual-host listener. Each named
positive must validate its peer identity; the server records the exact
requested name and selected certificate thumbprint. Missing and unknown
SNI select the default certificate and must fail real hostname validation,
with exact server-side evidence. Fresh default-trust client contexts avoid
session resumption masking a selection error.

The separate HTTP listener is essential: localhost can omit SNI, so sharing
it with the virtual-host listener would allow earlier HTTP traffic to supply
the empty-name evidence intended for the missing-SNI control. Root found
that false-positive path during review and the source separates the listeners.
All listeners/clients remain bounded and are disposed before certificate/
test-root cleanup, including exceptional paths.

## Provenance and pending gates

The negative-certificate slice originates in Web3 ec111f78, and the NetApp
slice in 60f1108; both are taken from frozen Web3 2755f21. Web3's red full
run is not reclassified. Media3 0148698/full40438975894 also remains red:
its 2025 suite has five external-network failures even though all media
checks pass. The new fixture source has not yet passed a full run.

The new coherent Media3 candidate includes current verified native clocks/
widths, the independently verified live-environ prerequisite and these exact
test bytes. Both Windows suites, selected-name/thumbprint/control checks,
package and both required e2e logs must pass before integration. This work
adds no network export or runtime implementation and closes none of Net4's
84 assigned libc/resolver or remaining socket/connectivity/crypto contracts.
