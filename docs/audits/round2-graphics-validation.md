# Round 2 graphics: damage propagation and display pacing

Date: 2026-11-23. Base: `bcfb5044029d486e17138dc417f3ced81524cf3f`.
Scope: GFX-04 and GFX-02/G07 from the #178/#56 handoff.

## Application contracts and behavior

CPU `ANativeWindow_lock` continues to report producer damage in top-left buffer
coordinates, expanding to the whole buffer after allocation/format/size changes.
The retained Android buffer still supplies pixels outside that region. For a
retained destination, only the relevant pixels are converted/copied to BGRA.
Insets crop the damage before it reaches the client area. Empty/invalid damage,
an intervening writer, a resized destination, or an unpreserved producer buffer
uses a full conversion. Cancelling a dequeued buffer invalidates its contents:
the producer may have modified it without posting those pixels.

`NATIVE_WINDOW_SET_SURFACE_DAMAGE` unions the producer's rectangles, converts
their bottom-left coordinates using the actual queued buffer height, and resets
the hint after each queue. CPU connections ignore this hint, using `lock`'s
damage instead. No new display capability is advertised.

The handoff's ownership list omitted `gesher_window.h/.cc`. Their existing
`UnlockAndPost()` interface discarded all damage, so a small additive rectangle
argument and retained-buffer version are necessary to carry correct damage to
Win32. The back-buffer mutex protects both the version and pending layered
damage; rendering threads do not touch GDI objects.

Regular windows invalidate the affected client rectangle. The `WM_PAINT` DC
clips the existing full-buffer GDI source to Windows' update region. Layered
windows retain a complete DIB, copy/dim only the union of pending damage, and use
`UpdateLayeredWindowIndirect.prcDirty`. The DIB size and source origin continue to
describe the whole window; a small dirty rectangle never resizes the popup.
First publication, size/display/style/dim changes and failed presentation force
a complete refresh. Duplicate queued messages after the latest damage was
already consumed do no DIB copy or presentation. DIB/DC ownership and teardown
remain on the window thread.

### Primary contract evidence

The following AOSP files were fetched directly from the pinned
`android-16.0.0_r4` tag on 2026-11-23; their behavior was inspected, not copied:

| Source | Relevant contract | SHA-256 of decoded source |
| --- | --- | --- |
| [system/window.h](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/nativewindow/include/system/window.h) | `native_window_set_surface_damage`: bottom-left origin, reset after queue, zero rectangles means full damage | `72b3276993c4de88cadb94079e9e201ff36615a3e0ced4dfcb9a3d743ad2fc88` |
| [Surface.cpp](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/gui/Surface.cpp) | `setSurfaceDamage`: CPU connection ignores hint; bottom-left input has `top > bottom`, temporarily represented by swapping top/bottom before queue conversion | `8d14b03ba40ede0d73d6b1b6a33fa5e460f357f6e1dc8d4b6ab507ea2464802a` |

The [NDK native-window reference](https://developer.android.com/ndk/reference/group/a-native-window)
defines the in/out dirty region of `ANativeWindow_lock` and preservation outside
that region. Microsoft's
[UPDATELAYEREDWINDOWINFO reference](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-updatelayeredwindowinfo)
defines `prcDirty` as the source area to update, independently of full-window
`psize` and `pptSrc`.

## Refresh pacing

Child EGL surfaces no longer impose an unconditional 16,667 microsecond period.
`MonitorFromWindow`, `GetMonitorInfoW` and `EnumDisplaySettingsW` supply the
current host monitor mode. The frequency is rounded to a nanosecond period;
unknown/default frequencies (0 or 1) and failed/headless queries fall back to
16,666,667 ns. An unchanged monitor is re-queried at most once per second, with
immediate re-query on a monitor change.

A child queue surface has no HWND or consumer-to-display association. Pacing
therefore prefers a foreground window only when it belongs to this process;
otherwise it uses the helper/default monitor. This is a host pacing choice,
not proof of the monitor on which every arbitrary SurfaceTexture consumer is
displayed. The helper stays hidden, its ANGLE swap interval stays zero, and the
existing high-resolution waitable timer performs the wait.

References: [MonitorFromWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromwindow),
[GetMonitorInfoW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmonitorinfow),
[EnumDisplaySettingsW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsw).

## Executed validation

Linux x86-64, GCC 14, ASan + UBSan. `ASAN_OPTIONS=detect_leaks=0` was required
because this container's ptrace environment prevents LeakSanitizer startup.
Address/undefined-behavior checks remained enabled. Resource counters separately
check DIB/DC lifetimes; they are not a process-wide leak detector.

| Command | Result |
| --- | --- |
| `python3 tools/test_surface_geometry_contracts.py --sanitize` | 158 geometry + 38 release + 75 damage checks, no failures |
| `python3 tools/test_layered_bitmap_cache.py --sanitize` | 1,441 checks, no failures; exact producer post, dimming, DIB copy and layered presentation methods |
| `python3 tools/test_surface_present_allocations.py --sanitize` | 38,028 checks, no failures; no intermediate frame allocations |
| `python3 tools/test_mouse_capture_contracts.py --sanitize` | 25 checks, no failures |
| `python3 tools/test_surface_frame_recycling.py --sanitize` | 331,204 checks, no failures |
| `python3 tools/test_video_scaling_contracts.py --sanitize` | 32,165 checks, no failures |
| `python3 tools/test_egl_readback_layout.py --sanitize` | 12,000 pixel-oracle comparisons plus 44 display-refresh boundary checks |

The existing surface/layered CI commands execute the new regression checks.
The existing EGL readback command also runs the display-refresh test; no
workflow is changed. The shared surface-geometry boundary is also consumed by
frame-recycling and video-scaling tests; both consumers were updated and run.

The unchanged damage assertions reject the base source (26 failures before
adding the CPU-connection assertions), and the layered assertions reject the
base source (5 failures before adding the duplicate-message assertion). These
are negative controls, not candidate failures.

LLVM-MinGW 20261006 compiled the real `gesher_window.cc` and `gesher_popup.cc`
translation units for Windows x86-64 and ARM64 with `-Wall -Wextra -Werror`.
The layered and display-refresh fixtures also cross-compiled for both
architectures, using real Windows type declarations. They were not executed on
Windows here. Full AOSP/ANGLE integration and physical display/GDI behavior
remain Windows CI/runtime checks; Linux host models do not establish them.

### Production `NOGDI` build regression

PR #391's full build exposed a missing compile condition in the earlier
standalone refresh check: `gesher_ndk_graphics` defines `NOGDI`, which hides
`DEVMODEW`, `EnumDisplaySettingsW` and `ENUM_CURRENT_SETTINGS`. In addition,
`egl_child_surface.h` includes `windows.h` before the pacing header, so changing
`NOGDI` inside that header would depend on include order.

The display query now lives in `display_refresh.cc`, which enables the SDK's
GDI declarations before its first Windows include. The public header and its
consumers retain `NOGDI`; no Windows struct or calling convention is redefined.
The implementation is an ordinary source of `gesher_ndk_graphics` and uses its
existing `user32` linkage.

`test_display_refresh_contracts.py` now detects Windows compiler targets and
also compiles and links the complete implementation with
`display_refresh_compile_test.cc`. That consumer includes `windows.h` first and
checks that `NOGDI` remains defined and GDI declarations stay excluded. The
compile uses the failing production flags, including `-std=gnu++20`,
`-mno-ms-bitfields`, `-fno-exceptions`, `-DNOGDI`, `-DNOMINMAX`,
`-DWIN32_LEAN_AND_MEAN`, the Windows 10 version defines and warnings as errors.

Both `x86_64-w64-mingw32-clang++` and `aarch64-w64-mingw32-clang++` from
LLVM-MinGW 20261006 passed this real SDK compile/link check and the recording
fixture cross-compilation. The same consumer rejected the original inline
header with the two CI errors before the repair. The unchanged Linux ASan/UBSan
suite still passes 12,000 readback comparisons and 44 refresh checks. These are
targeted build and behavior regressions; the cross-compiled executables were
not executed on Windows.

```sh
python3 tools/test_display_refresh_contracts.py \
  --cxx "$LLVM/bin/x86_64-w64-mingw32-clang++" \
  --compile-only --output /tmp/display-refresh-x64.exe
python3 tools/test_display_refresh_contracts.py \
  --cxx "$LLVM/bin/aarch64-w64-mingw32-clang++" \
  --compile-only --output /tmp/display-refresh-arm64.exe
```

## ABBA host measurement

Command: `python3 tools/test_surface_geometry_contracts.py --measure-only`,
adding `--source-ref bcfb5044029d486e17138dc417f3ced81524cf3f` for each base run.
Order: candidate, base, base, candidate. Same host, fixture and compiler flags.
Each sample presents 400 retained 1920×1080 RGBA frames; the producer changes
the same 64×64 region. The `full` scenario declares full damage, while `64×64`
declares that region. All final checksums were `ffe53512`.

| CPU presentation total (ms) | Candidate 1 | Base 1 | Base 2 | Candidate 2 |
| --- | ---: | ---: | ---: | ---: |
| Full damage | 1,046.133 | 861.443 | 989.577 | 959.723 |
| 64×64 damage | 2.408 | 980.107 | 886.432 | 2.368 |

These measurements cover producer locking/drawing and the real CPU conversion
loop with an in-memory Window boundary. They exclude GDI/compositor, GPU,
desktop refresh, and physical hardware; no end-to-end frame-rate gain follows
from the table. The retained partial case touches 4,096 pixels instead of
2,073,600 pixels per frame, explaining the large CPU-work difference.
The shared host shows substantial full-frame timing variation; this table does
not establish a full-frame speed improvement or an end-to-end latency budget.
