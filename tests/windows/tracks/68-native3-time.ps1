# ART's elapsed clocks must share the native monotonic domain at all scales.
# The host fixture links the actual patched artbase; its independent unbiased interrupt-time bracket
# rejects the former Unix wall-clock fallback without changing the system clock.
Check 'native3-time: ART elapsed clocks use unbiased interrupt time, units and concurrent ordering' {
  N2Run "$n2Dir/native3_cpu_clock_test.exe" @()
} @('PASS  native3 clocks: ART nanoseconds match the independent unbiased interrupt-time bracket',
  'PASS  native3 clocks: ART microseconds match the independent unbiased interrupt-time bracket',
  'PASS  native3 clocks: ART milliseconds match the independent unbiased interrupt-time bracket',
  'PASS  native3 clocks: the monotonic bracket rejects the old wall-clock domain',
  'PASS  native3 clocks: all ART monotonic clocks keep their order and units',
  'PASS  native3 clocks: the ART elapsed clock advances across Sleep',
  'PASS  native3 clocks: concurrent callers share the unbiased interrupt-time clock domain',
  'PASS  native3 clocks: absolute monotonic deadlines use unbiased interrupt-time and normalize their offset',
  'PASS  native3 clocks: absolute realtime deadlines use UTC and normalize their offset',
  'PASS  native3 clocks: relative deadlines retain their duration without an epoch', '0 test(s) failed')

# No simulated suspend is counted as hardware evidence. The optional native
# --after-suspend mode is for an explicitly recorded physical acceptance run.
Check 'clock domains: Windows unbiased/biased readers and unchanged host pthread epoch' {
  N2Run "$n2Dir/clock_domains_windows_test.exe" @()
} @('clock-domains-windows: PASS MONOTONIC matches independent unbiased bracket',
  'clock-domains-windows: PASS BOOTTIME matches independent biased bracket',
  'clock-domains-windows: PASS host winpthreads keeps its QPC epoch',
  'clock-domains-windows: PASS explicit Android adapter keeps the awake epoch',
  ' assertions, 0 failures')

# Default mode runs only awake waits; physical --after-suspend qualification is
# opt-in and must be recorded separately for suspend and hibernate.
Check 'boottime transport: real Windows registration and read poll epoll guest sleep' {
  N2Run "$n2Dir/boottime_resume_windows_test.exe" @()
} @('boottime-windows: PASS blocked timer read receives one expiration',
  'boottime-windows: PASS guest BOOTTIME sleep completes without EINTR',
  'boottime-windows: PASS blocked native poll wakes',
  'boottime-windows: PASS blocked native epoll wakes',
  ' assertions, 0 failures; physical=no')
