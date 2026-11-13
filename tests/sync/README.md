# Sync host contracts and the PR15 coexistence probe

`tools/test_sync_manager.py` normally compiles the current production SyncManager
and runs the shared service-registry fixture over the declared host JNI boundaries.
`--source-ref` selects a separate baseline control. The default CI command is unchanged.

`--idle-policy-probe` is an explicit additional check for the application built by
combining this change with the idle policy from
[PR15](https://github.com/gesher-runtime/gesher/pull/15). It loads **all production
classes from the supplied application jar**, without compiling a SyncManager
replacement. It rejects a baseline-control argument or a partial test group, and
fails with an explicit prerequisite error if the production idle-policy methods
are missing. `SyncIdlePr15Probe.java` is outside the ordinary services DEX build.

## Reproduce the combination

The independent 2026-11-13 run combined candidate production source at local
revision `8b2e4fef15cb8be2fc518c641ec67a1c2d81bd96` with the complete PR15 head
`d2e11ea48580e46cba48156aafa8308476e39813`. The same candidate production changes
are in this branch. The final shared fixture includes QA commit `68760a68`.
Both combinations must be freshly rebuilt if their production sources change.

From a checkout of this branch, prepare a separate combined worktree:

```sh
git fetch origin refs/pull/15/head:refs/remotes/origin/pr-15
test "$(git rev-parse origin/pr-15)" = d2e11ea48580e46cba48156aafa8308476e39813
git worktree add --detach ../gesher-sync-pr15 HEAD
git -C ../gesher-sync-pr15 merge --no-commit --no-ff origin/pr-15
```

Use the normal documented JDK, R8, apksig, pinned AOSP and framework/libcore build
inputs. Set `SYNC_QA_JAVA_HOME`, `SYNC_QA_FRAMEWORK`, `SYNC_QA_CORE`,
`SYNC_QA_APKSIG`, and `SYNC_QA_OUTPUT` to their absolute paths; the last is a new
output directory. `AOSP_DIR`, `R8_JAR` and the JDK on `PATH` are the existing app
build inputs. Build the **whole combined application**, then invoke the probe:

```sh
APKSIG_JAR="$SYNC_QA_APKSIG" bash ../gesher-sync-pr15/app/build.sh "$SYNC_QA_OUTPUT" \
  --framework-classes "$SYNC_QA_FRAMEWORK" --core-classes "$SYNC_QA_CORE"

python3 tools/test_sync_manager.py --idle-policy-probe \
  --java-home "$SYNC_QA_JAVA_HOME" \
  --framework-classes "$SYNC_QA_FRAMEWORK" --core-classes "$SYNC_QA_CORE" \
  --app-classes "$SYNC_QA_OUTPUT/gesher-app-classes.jar" \
  --apksig-jar "$SYNC_QA_APKSIG"
```

The restored-code run passed **319 ordinary Sync checks and 11 additional idle-policy
checks**. The probe observes a real service binding with its activity closed,
checks PR15's background-service/idle classification, and requires the active sync
to preserve device lifetime after ten idle timeouts. Completion must release the
binding and idle protection. A future retry preserves the first idle grace interval,
permits shutdown after twice the timeout, and releases all grace after cancellation.
The manually dispatched manager uses the actual host elapsed-clock domain for
these future-deadline comparisons.

This is host behavior evidence using the actual merged Java production classes and
declared JNI identity/allocation/clock boundaries. It does not execute Windows ART,
cross-process Binder, or persistent-device process shutdown. Those remain Windows
CI gates. The probe does not merge PR15 into the submitted branch.
