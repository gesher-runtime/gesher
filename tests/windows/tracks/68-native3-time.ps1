# ART's elapsed clocks must share the native monotonic domain at all scales.
# The host fixture links the actual patched artbase; its independent QPC bracket
# rejects the former Unix wall-clock fallback without changing the system clock.
Check 'native3-time: ART elapsed clocks use monotonic QPC time, units and concurrent ordering' {
  N2Run "$n2Dir/native3_cpu_clock_test.exe" @()
} @('PASS  native3 clocks: ART nanoseconds match the independent QPC bracket',
  'PASS  native3 clocks: ART microseconds match the independent QPC bracket',
  'PASS  native3 clocks: ART milliseconds match the independent QPC bracket',
  'PASS  native3 clocks: the monotonic bracket rejects the old wall-clock domain',
  'PASS  native3 clocks: all ART monotonic clocks keep their order and units',
  'PASS  native3 clocks: the ART elapsed clock advances across Sleep',
  'PASS  native3 clocks: concurrent callers share the QPC clock domain',
  'PASS  native3 clocks: absolute monotonic deadlines use QPC and normalize their offset',
  'PASS  native3 clocks: absolute realtime deadlines use UTC and normalize their offset',
  'PASS  native3 clocks: relative deadlines retain their duration without an epoch', '0 test(s) failed')
