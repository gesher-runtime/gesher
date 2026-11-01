# Actual bionic core with an independent owned Win32 handle oracle.
# Host winpthreads API qualification remains a separate pending requirement.
Check 'pthread CPU target: retained identity, native brackets, capacity and handle reclamation' {
  N2Run "$n2Dir/pthread_cpu_clock_target_test.exe" @()
} @('PASS  pthread target core: 257 real external self identities remain live',
    'PASS  pthread target core: invalid identities and IDs preserve outputs',
    'PASS  pthread target core: 256 live slots and resource exhaustion preserve output',
    'PASS  pthread target core: 128 busy target samples match independent owned Win32 handle brackets',
    'PASS  pthread target core: 1024 acquisitions allocate no additional handles at capacity',
    'PASS  pthread target core: all external targets actually exit within one deadline',
    'PASS  pthread target core: exit reaping restores capacity',
    'PASS  pthread target core: retired ID cannot alias the newly acquired caller clock',
    'PASS  pthread target core: terminated external and clock handles are reclaimed',
    '0 test(s) failed')
