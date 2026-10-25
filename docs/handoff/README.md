# Handoff: closing the known gaps (wave after PR #3)

Everything up to "Phase 3+" is on `main` (PR #3, merge 86aa4c7): ART with
nterp/JIT/AOT and a boot image, the framework, Gesher's own system server and
app processes, Win32 app windows, input, fonts, the NDK (ELF loader, bionic
shim, NativeActivity), the installer and an e2e job with pinned F-Droid apps.

The project owner's order after that: **close every known gap, and don't stop
until none are left.** The gaps run as parallel tracks, each on
`wip/<track>` cut from main 86aa4c7, each in its own worktree
`C:\work\gesher-<track>`. Every track agent follows
[track-rules.md](track-rules.md); each track's original brief is in
[tracks/](tracks/).

## Integration branch

`phase4/integration` (cut from main, worktree `C:\work\gesher-int`). Merge
only **CI-verified SHAs** (both test runners, package and e2e green), never a
branch tip that has commits CI hasn't run. A docs-only commit on top of a
verified SHA is fine.

Merged so far:
- popups: 9edfef6 (code verified at 6ce1d20, ci 40129561197). ADR 0040.
- rtl: 3aae3be (verified at 1c9b6b2, ci 40133992911). ADR 0044.
- Integration ci run 40135350639 on 6cea4ca (popups + rtl): all green.
- exec: merged (verified at 059b267, ci 40137662101; Puzzles passes e2e).
  ADR 0035.
- devsvc: merged (verified at a02e84a, ci 40137762701 attempt 2; attempt 1's
  windows-2022 failure was a one-off rtlapp-under-ar-EG flake, passed on
  rerun: watch for it). ADR 0038.
- webview: merged (verified at 9c610d6, ci 40139785905; Notes passes e2e).
  ADR 0041.
- stack: merged (verified at 29c5255, ci 40141531509, which already
  contained every merge above, so the integration tree after this merge is
  the CI-verified one: 296 PASS lines per runner, up from 263 at 40135350639;
  the "408" in older briefs counted differently). ADR 0036.
- devices: merged (verified at f62f1aa, ci 40142590418; Camera2 verified
  against a test Media Foundation source, no real camera on the runners).
  ADR 0043. Integration ci 40143864548 on 6111f0d (all seven merged
  tracks together): all green.
- 2026-10-25: every agent stopped on the org's API spend limit; all were
  resumed with SendMessage after it reset (their work was saved).
- locale: merged (verified at 0946451, ci 40191538466, which already
  contained 26aa8eb, so the integration tree after this merge is the
  CI-verified one). ADR 0049. Other tracks adding services must keep
  `locale` in 50-services.ps1's expected list. Leftovers: no activity
  recreate in single process, LocaleConfig overrides not persisted, no
  ACTION_APPLICATION_LOCALE_CHANGED, display-language list changes untested
  (need a sign-in).
- New wave-2 track: `webview2` (WebView leftovers, ADR 0050), on
  wip/webview2 in the C:\work\gesher-webview worktree.

In `framework.yml` every new test app is built and copied on its own line, so
merges only add lines.

## Where each open track stopped (2026-10-25)

All branches are pushed; all worktrees are clean. Each track doc has a
"Status and next steps" section on its branch (the status of the stack,
webview and exec tracks may be less fresh: those agents were stopped
abruptly; check `git log origin/main..` on their branch).

| Track | Branch tip | Last verified | Status doc | Where it stands |
|---|---|---|---|---|
| exec (ELF processes, Puzzles; ADR 0035) | cd27227 | earlier runs had tests green on both runners; e2e for Puzzles was being checked | `docs/exec.md` | read the latest ci run on `wip/exec`; finish Puzzles in e2e |
| stack (SOE under nterp/JIT/AOT, JNI stubs, placeholders, dual JIT cache; ADR 0036) | 916f7ec (last commit is WIP, uncommitted work saved at stop) | none known | `docs/compiled-code.md` | compiled JNI stubs, placeholder mappings, oat views, dual-mapped JIT cache committed; SOE and math-entrypoint checks in progress |
| net (BoringSSL, Conscrypt, Windows roots, ConnectivityManager; ADR 0037) | 32f5660 | compile-only net.yml green at b3cca77 (40128266078) | `docs/network.md` | ci 40133972018 at 44a2e95: build, package, e2e green; test jobs' result unread |
| devsvc (alarms, jobs, clipboard, toasts, battery, wake locks; ADR 0038) | bee6187 (code 68a913f) | none fully; 40133991534 at 30ba276 had 8/10 new checks green, both failures fixed in 68a913f | `docs/services.md` (devsvc section) | read ci 40135237081 at 68a913f; if green, that's the verified SHA |
| ime (IMM32 composition, InputMethodManager, layouts; ADR 0039) | f0bc81a | none; 40134234988 green on windows-2025, windows-2022 hangs after the Japanese IME step | `docs/input.md` (ime section) | three commits with the likely fix not yet run; already merged with phase4/integration (popups) |
| webview (WebView2; ADR 0041) | 93a384e | see `docs/webview.md` | `docs/webview.md` | already merged with phase4/integration (popups); Notes (WebView) added to e2e |
| media (WASAPI, AudioTrack, AudioManager, codecs; ADR 0042) | c729b00 | da91170, ci 40129230078 (audio engine, AudioTrack, AudioManager) | `docs/media.md` | MediaPlayer/SoundPool/codec build (882a1af, run 40135129611) failing in the codec job; F-Droid audio app SHA missing |
| devices (location, sensors, Camera2; ADR 0043) | 9c684f2 (last commit is WIP) | ci 40134370867: location and sensors on both runners | `docs/devices.md` | Camera2 over Media Foundation in progress (Java side: WindowsCamera, CameraFrames) |
| posix (paths, /proc, tzdata, inotify, process model, ART gtests; ADR 0045) | 2152bcb | none; runs 40135642983 (ci) and 40135639733 (posix.yml) at 65cccdc unread | `docs/posix.md` | read those runs first |

Coordinator session 2 (2026-10-25) read those runs: integration 40135350639
had both test jobs green (e2e still running at the time); net 40133972018:
test jobs still in progress (possibly hung); devsvc 40135237081: both test
jobs failed; media 40135129611: all green except `media / codecs`; posix
40135642983/40135639733: compile failure in `build`. A successor agent was
started for each of the nine tracks above with those findings.

New tracks of wave 2 (cut from phase4/integration, briefs in
[tracks/](tracks/)): `arm64` (Phase 7, ADR 0047, tests 47-arm64.ps1),
`lifecycle` (popups leftovers, ADR 0048, 48-lifecycle.ps1), `locale` (rtl
leftovers, ADR 0049, 49-locale.ps1). Media stage 2 (video) goes to the media
track after its stage 1 is merged; ime on windows-2022 is the ime
successor's job. The roadmap box "the framework's own system services" is
closed by decision in ADR 0046 (Gesher's own server, ADR 0019).

Leftovers reported by finished tracks (for a later wave):
- popups: the system server stops (Android would only pause) the Activity
  under a translucent or dialog-themed one; popups are opaque under SkiaGL;
  no real `ACTION_OUTSIDE`; minimizing doesn't stop the Activity.
- rtl: no per-app locales; Windows setting changes don't reach running apps;
  decimal/grouping symbols and date patterns not carried over.
- posix: a process killed by a signal exits with 128+signal; exec's `waitpid`
  must decode it the same way. `persist.sys.timezone=UTC` in build.prop is
  unused today and would override Windows' zone if ever read.
- Not yet assigned at all: Phase 7 (arm64 translation); the roadmap box
  "the framework's own system services" (by design Gesher has its own server,
  ADR 0019: decide whether to reword or close it).

Expected merge conflicts: `Services.java` registration lines (net, devsvc,
ime, media, devices), `ci.yml` jobs/`needs` and artifacts (net, media, posix,
webview), `framework.yml` test apps, `CMakeLists.txt` includes, `aosp.lock`,
`compat/framework/android_runtime.cc` rows, `libcore_native.cmake` (exec and
posix), `compat/posix/src/signal.cc` (stack and posix), `tools/package/*`,
`docs/roadmap.md`, window code (ime/webview already merged popups).

## The project owner's working principles

- Replies to the owner in **Hebrew**; code, commits and docs in English, in
  the existing style. Commits end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, PR descriptions
  with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- **CI first**: never build AOSP locally; WSL only when there is no other way
  (7 GB RAM; an OOM once rebooted the machine). Disk is limited.
- **Parallel tracks** in disjoint file domains, each one subagent in its own
  worktree. One agent per whole task (not tiny milestones, not huge). A
  stopped agent's successor starts from its brief plus its status section.
- **Every agent runs on Opus**: pass the model explicitly when starting one;
  never let it inherit another model.
- **Keep going without asking** until the goal: a complete, ready installer
  with every gap closed.
- **Honest reports**: tick roadmap boxes only for what CI verified.
- **Merging to `main` only with the owner's explicit approval**, each time.
- Never touch `C:\work\gesher-backup-2026-10-23`.
- A word-based network filter (NetFree) sometimes blocks text or requests:
  rephrase, or write the text to a file.
- **GitHub API rate limit**: a dozen agents polling with `gh run watch`'s
  default 3 s interval exhausted it on 2026-10-24. Always use
  `gh run watch <id> -R gesher-runtime/gesher --exit-status --interval 120`,
  and on HTTP 403 back off ~10 minutes.
