# Track brief: posix

posix layer leftovers. The original brief given to the first agent of this track (2026-10-24), kept verbatim. A successor reads it together with the "Status and next steps" section in the track's doc on its branch.

You are the "posix" track agent of the gesher project. Your worktree is C:\work\gesher-posix on branch wip/posix (already created from main 86aa4c7 and pushed). Work only there.

FIRST read C:\work\gesher-posix\docs\handoff\track-rules.md and follow it exactly: it is binding (CI-first, never build AOSP locally, commit trailer, honesty, final report format, stay in your domain). Main contains everything up to "Phase 3+". Read docs/roadmap.md Phase 1, docs/decisions/0005 (posix layer), 0008 (linux ABI), 0009 (socket fds), 0010 (paths), docs/core-libs.md, compat/posix.

GOAL: close the open Phase 1 boxes of the gesher-base POSIX layer and libcore that no other track owns:
1. Time zone data: ANDROID_TZDATA_ROOT with libcore's tzdata/ICU zone data from the pinned AOSP (system/timezone), TimeZone.getDefault() following Windows' zone (Windows zone id -> IANA via ICU's windowsZones or the Windows API), java.time and android.icu agreeing, DST transitions correct; shipped by the installer.
2. Paths: drive letters other than the current drive inside ART's ':'-separated lists (boot class path, library paths, class path) and in general paths (decide a mapping such as /mnt/d/... or Gesher's mount table, record it in ADR form), and the rest of the CRT's path functions (fopen, unlink, rename incl. replace-existing semantics, access, stat/lstat/fstatat, mkdir, readlink/symlink/realpath, opendir) going through gesher-base's path model consistently, with long paths (\\?\) and non-ASCII names.
3. inotify (FileObserver) over ReadDirectoryChangesW, epoll-compatible.
4. A process model leftovers: getpid/getppid/kill to other gesher processes, getuid per app if the services track set one, /proc/self/* entries apps read (status, maps, fd), sysconf/getrlimit sanity.
5. ART's own gtests for libartbase and libdexfile running on Windows in CI (a reasonable subset, exclusions documented like docs/core-libs.md does for libutils).
Do NOT work on: TLS slot/thread register, signals/VEH, MAP_FIXED/placeholder mappings, executable mappings (the "stack" track owns those), process spawning/exec (the "exec" track), BoringSSL/networking (the "net" track).
Tests in tests/windows/tracks/21-posix.ps1 (new; plus probes under probes/posix) and the gtests job; both runners.
Your file domain: compat/posix (excluding signals/mmap/thread_register), compat/libcore (non-networking, non-process), probes/posix, tests 21-posix.ps1, the gtests workflow pieces (in libs.yml additively or your own posix.yml), docs/core-libs.md and a new docs/posix.md, ADR docs/decisions/0045-posix-*.md, the Phase 1 roadmap boxes you close.

Coordinator notes: ten other tracks run in parallel on the shared CI (exec, stack, net, devsvc, ime, popups, webview, media, devices, rtl). compat/posix is also touched by stack (signals, mmap, threads) and exec (process spawn): stay off those files. Local disk is low (~11 GB free shared by all agents): keep local downloads small, delete them when done, never build AOSP locally. Trigger CI only when code is ready, cancel superseded runs, rerun only failed jobs on transient errors. Keep a "Status and next steps" section in your doc updated with every meaningful push so a successor could continue from it alone. Do not stop at a partial result: finish to CI-verified green on both runners. Final message = the report track-rules.md asks for.
