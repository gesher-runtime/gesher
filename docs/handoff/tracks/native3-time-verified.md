# Native3-time: verified elapsed and deadline clocks

Source bdfb230fa3d70797caece6568b6ac529e854b4ee, full ci 40434741055,
attempt 1. Root personally inspected all 28 job conclusions (22 success/six
skipped), both zero-failure Windows summaries, actual new clock/CPU/ABI PASS
lines, package install/window/input/uninstall and both required nine-app
e2e logs including Puzzles/TIC-80 (zero unexpected). No retry was requested.

ART0573 selects the existing CLOCK_MONOTONIC branches for NanoTime,
MicroTime and MilliTime, and honors InitTimeSpec's clock argument. Its
previous Windows path ignored that argument and used UTC even for a
monotonic deadline. The unused wall helper is excluded only after every
Gesher caller has selected the proper clock path.

The actual linked-artbase fixture verifies all three scales against
independent Win32 QPC brackets, ordered samples, elapsed sleep, concurrent
callers, absolute QPC/UTC deadlines with offsets and a deterministic relative
rollover. A UTC counterexample rejects the old epoch domain. It retains all
five independent current-thread/process/worker CPU checks. No system clock
is changed. The actual main log applies0573 and compiles time_utils.cc plus
the fixture under -Werror; both real Windows test68 aggregates pass.

First source c168b23/ci40433469320 was cancelled after independent source
review found an unconverted InitTimeSpec call to the excluded helper.
The correction and direct deadline assertions passed a second source review
before this accepted run. No result from the superseded source is accepted.

Tests121973635111/121973635241 both end with zero failures and keep actual
signed/unsigned NDK whole-range width and four APK-mode checks green. Original
startup profile/speed medians: interpreter560/598 and JIT436/469ms on2022;
interpreter414/446 and JIT330/369ms on2025. Original size/timing gates remain
unchanged. Package121973635173 verifies installed oatdump/image/fonts,
install/dexopt, associations/shortcuts, real window/input/resize and full
process/data/toast/Start-menu uninstall cleanup. E2e121974272709/121974272622
pass all nine apps, four transport fixtures and association/shortcut launches.

Only documentation may be added to the frozen source. Width/CPU prerequisite
reports from c8f30ec are already incorporated with no code difference above
the verified bdfb230. Root must compare the integration result against this
source and honor rule g for any changed combination. Non-current pthread
clocks, native allocator accounting and remaining native3/POSIX scope stay
open. This is no camera-performance or release-completion claim.
