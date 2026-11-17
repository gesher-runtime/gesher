# Android clock-domain contracts

Run `python3 tools/test_clock_domains.py --sanitize` from the repository root.
The fixture compiles the production clock helper, Bionic clock and sleep
adapters, futex and POSIX deadlines, audio/video pacing, MediaCodec callbacks,
and patched AOSP clock functions. Operating-system and JNI/video transport
boundaries are controlled. Awake time,
suspended time, wall time, CPU time, and the host pthread/QPC epoch are distinct
in the fixture; advancing one cannot accidentally advance another.

The AOSP files in `aosp/` are unmodified Apache-2.0 sources from the repository's
`android-16.0.0_r4` pin. The runner checks both the release pin and these hashes,
then applies the actual repository patches with `git apply
--whitespace=error-all` before compiling their function bodies:

| File | Upstream path | SHA256 |
| --- | --- | --- |
| `time_utils.cc` | [art/libartbase/base/time_utils.cc](https://android.googlesource.com/platform/art/+/refs/tags/android-16.0.0_r4/libartbase/base/time_utils.cc) | `57d9da8802ff4313e65aec616af020bee27f788f4a6f9104b68033217cc0bb6b` |
| `Timers.cpp` | [system/core/libutils/Timers.cpp](https://android.googlesource.com/platform/system/core/+/refs/tags/android-16.0.0_r4/libutils/Timers.cpp) | `43657a80d3cac5edc1d88a7933ed8970a0c3f6a2542cb9892cd904003704f745` |
| `SystemClock.cpp` | [system/core/libutils/SystemClock.cpp](https://android.googlesource.com/platform/system/core/+/refs/tags/android-16.0.0_r4/libutils/SystemClock.cpp) | `2e4d1d550d9bcabf52ea11a3aef4bb05928cdc0875266fdb715afb23e7803da7` |

Refresh the files and hashes deliberately when updating the AOSP pin. The host
fixture selects the Windows/GESHER preprocessor branches after loading its
Linux transport headers. It must not use the Linux `SystemClock` branch to
stand in for the Windows fallback.

## Regression evidence

At the implementation base, this command compiles successfully but fails 30
of 71 assertions because the old code mixes the QPC and Android clock domains:

```sh
python3 tools/test_clock_domains.py --sanitize \
  --source-ref 6f5e23a598c10906e4518045bc8d73f5c0cf3a9f
```

The corrected timerfd fixture models native relative-timer eligibility across
suspension and delivers a distinct power notification. Its `suspend` case fails
11 of 31 assertions on the reviewed PR head (which already has correct readers):

```sh
python3 tools/test_timerfd_contracts.py --sanitize --case suspend \
  --skip-linux-reference --source-ref 35c55f3b7024fd5f0b7cd91f2b46b77f87aadd26
```

The candidate passes all 71 clock assertions, 31 timerfd suspend assertions and
35 integrated sleep/resume assertions (`tools/test_boottime_resume.py`). These
are deterministic regression tests, not physical suspend
qualification. See [the acceptance notes](../../docs/research/clock-domains-2026-11-17.md)
for the real Windows probe and remaining runtime gates.

For old-source controls, references to `std::chrono::steady_clock` are redirected
to the same controlled host-QPC boundary as native QPC calls. This preserves
their production duration arithmetic while making simulated suspension visible
to the former clock, including video-only pacing.
