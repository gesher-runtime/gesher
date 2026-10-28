# Canonical full test77 dual entry (pending new CI)

Small owner delta above fec66a76/00c7af77: test77 itself now invokes every
original assertion for both absolute and genuine relative Dir entries, both
x86_64 and arm64-v8a. Four original checks per entry give8passes/host.
It pins the parent working directory and restores caller Dir/cwd/trace in
finally. Self-program Resolve-Path still precedes NoDrive. No assertion,
production ordered-action/child127/parent-isolation semantics or deadline is
changed. Scoped driver calls that same full entry once, avoiding duplication.

Earlier exact source00c7af77 scoped40549932840 already proved8passes/host;
this later full-entry change is not covered by that older proof and needs
its own CI plus the future changed-union qualifying full. The owning branch
also contains unmerged ordered-spawn/action/ARM-flag production scope; do not
mistake the whole branch for only a path or test-loop correction. Future
Native record-lock spawn wiring must coordinate that exact interface first.

## Exact later component proof

Source82d41d2117ea587876e3cc1a153dc6486c8b4d6a/run40582080725 attempt1:
4jobs green, actual build122463847311 andNDK122463847713 logs read;
actual Windows2022 job122465732152 and2025 job122465732026 each show
absolute rooted=True and relative Dir=dist rooted=False, all8 original
ordered-action/child127/parent-isolation/no-console passes and0failures.
Fresh launcher/no-console artifact12769373520 andNDKexec12769409392.
Plain logs retained prefix C:/work/gesher-ci-review/exec4-full-entry-40582080725-
with build.log,ndk.log,test2022.log,test2025.log,result.json,watch.log.
This validates the canonical shared entry at component scope only. Future
merged full must actually show8passes/host; no postmerge/full equality claim.
