# aot3: verified Gesher startup-profile coverage

Source ca46e9967f8994fe329f3f10c88de7141e494f76, full ci 40414859801.
Root personally read all 28 jobs (22 success/six skipped), both zero-failure
Windows summaries, all 18 aot3 PASS lines per runner, actual package
window/input/uninstall cleanup and both required Puzzles/TIC-80 e2e logs
(0 unexpected failures). No rerun was requested.

## Profile input and independent inspection

Fast ci 40410450314 records actual startup profiles from both Windows
runners with explicit runtime/script provenance. Their normalized method
sets match. The supplement adds 1971 observed framework/core boot methods
absent from the pinned AOSP profiles and 177 observed declaring classes;
it retains H/S/P flags and omits inline caches. Gesher/app methods are
excluded because gesher-app.jar already uses speed. The input SHA-256 is
053e8d37c550f79720919fe92cfbc559c9c28a2a7f7ccfeee3641a7098bc3c59.

Boot-image CI combines the supplement with both original pinned profiles.
The actual shipped framework oat contains independently inspected compiled
code for PackageParser2.parsePackage and SystemFonts.buildSystemFallback.
Existing cloud-profile/oatdump/collector checks still pass. The standalone
probe workflow uses CI artifacts only and does not replace the full gates.

## Original size and performance gates

Both runners report framework oat 9.3 MB versus 72.2 MB speed, all oats
17.1/96.5 MB and the complete image 75.3/162.6 MB. The original size limits
are unchanged. Original startup measurements use an excluded warmup, five
samples and interleaved pair order; the 10%/30ms comparison remains intact.

| Runner | Interpreter profile/speed, ms | JIT profile/speed, ms |
|---|---|---|
| Windows-2022 | 570 / 611 | 427 / 432 |
| Windows-2025 | 531 / 557 | 387 / 411 |

These are measured comparisons against the same run's speed image, not a
controlled before/after comparison against the previous profile or proof
that timing variance is eliminated. Future combined runs must keep the
same assertions and diagnose any real regression.

## Full installer and e2e evidence

Test jobs 121897553761/121897553839 both end with zero failed tests.
Package 121897553784 inspects installed oatdump/image/fonts, install/dexopt,
shortcuts/association, the real UI window and input/resize, then process/data/
toast/association/Start menu uninstall cleanup. E2e 121898378403/121898378451
passes all nine apps including required Puzzles/TIC-80, four transport
fixtures and association/shortcut launches, with zero unexpected failures.

## Merge and remaining contracts

This source predates the concurrent exec3 integration. Merging it with that
newer code requires combined integration CI under rule g before another
track merge. Only documentation may be added to this frozen source.

That combination now passes full integration ci 40432541082 at 42cc1c0.
Root read all 28 conclusions, every 18 aot3/11 exec3 scoped PASS line per
runner, both zero-failure summaries, actual package and both required e2e
logs (zero unexpected), without a retry. Original startup medians are
interpreter 364/386 and JIT 301/326 ms on 2022; interpreter 610/634 and JIT
447/476 ms on 2025. Original size/performance assertions remain unchanged.

Shared-library loader contexts and secondary dex/profile/invalidation are
still open on aot3/ADR0065. ARM width/CPU clocks, media, Windows input and
the remaining tracks are independent contracts. No final main merge or
0.1.0 publication is implied by this scoped stage.
