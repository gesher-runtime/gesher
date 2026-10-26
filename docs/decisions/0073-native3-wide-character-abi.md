# 0073. Native3 wide-character and multibyte ABI completion

Status: proposed. Implementation and runtime proof belong to
[native3-wide](../handoff/tracks/native3-wide.md); no completion is claimed.

Android LP64 uses 32-bit wchar_t, 64-bit long and an 8-byte conversion state;
the Windows CRT is not an ABI-compatible substitute. Complete the shim's
restartable conversions, wide integer/float/locale/time paths using the pinned
bionic contract, with errors, state, pointer movement and bounds tested through
both native ABIs and actual ART/APK callers. Keep one effective implementation
per registered function rather than relying on duplicate fallback order.

Unicode property/case/width equivalence has separate pinned-oracle proof.
Binary128 conversion/libm and wide scanf remain separate native3 work; this
decision cannot accept them through a binary64 or 16-bit CRT approximation.
Acceptance requires the complete full-CI and installer/e2e gates in the brief.
