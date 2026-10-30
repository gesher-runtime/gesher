# TCP reboot successor

2026-10-30. Original owner was interrupted by machine reboot. Isolated successor tree `C:/work/gesher-tcp-reboot`, branch `wip/tcp-priority-reboot`, preserves original clean50042 ancestry without editing its tree/ref.

Independent plain logs from WinSDK characterization40972149179 on both hosts show literal INVALID_SOCKET returns rc-1/WSAENOTSOCK10038, with and without handle tracing. This is distinct from tracing-induced0xc0000008 for physically closed positive socket handles. Accordingly, only the stale internal unowned-native sentinel fixture expectation changes from EINVAL to ENOTSOCK. rc-1/no synthetic readiness, all owned POSIX NVAL/count/deadline controls, handle censuses, cancellation/alias and UDP payload assertions remain unchanged. No production source changes.

The component push trigger and private supplier exact-ref assertion are adapted to the isolated successor branch only. All source/run/attempt/compiler/supplier/role receipts remain required. Independent peer review precedes publication. Fresh current-source component must reach HOST and both guest ABIs on both Windows hosts; prior red409708 is preserved. A new coherent full remains required before any shared-socket product adoption. No source or CI acceptance is claimed by this preparation.
