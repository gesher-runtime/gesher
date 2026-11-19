# Account session lifetime and timeout regressions

Run `tools/test_account_session_lifetime.py` with `--java-home`,
`--framework-classes`, `--core-classes`, `--app-classes` and `--apksig-jar` pointing
to the usual local compile inputs. On Windows also supply `--cc` with llvm-mingw.
`--source-ref origin/main` compiles the unchanged service as a negative control.

The runner compiles the entire production AccountManagerService and constructs
its real nonpersistent service with an empty PackageManagerService registry.
Synthetic authenticator metadata supplies a stable service identity; the existing
JNI fixture supplies a local system caller, clocks and Binder-holder allocation.
Production Answer, callback authentication, result/error delivery, cancellation,
timer scheduling and terminal cleanup all execute unchanged.

Checks cover success, errors, invalid results, package removal, explicit timeout
execution, replay rejection, reentrant synchronous completion, queued initial
results, and completion racing a blocked timer schedule. The latter injects a
ScheduledThreadPoolExecutor subclass as the declared scheduling boundary, blocking
its construction-time schedule call while the completion thread starts. Cleanup must leave the real
timer queue empty and clear listeners, results and pending bundles. The baseline
executor is unwrapped reflectively so the negative control inspects its actual
queue instead of merely testing the executor's type.

`AccountSessionTimeoutHost` also exercises the production initial wait and chooser
continuation wait helper. A private server-local uptime supplier is set only by
reflection in the fixture; production uses `SystemClock.uptimeMillis`. The tests
advance that clock and invoke the actual queued ScheduledFuture, rather than
sleeping for fifteen minutes or replacing the timer callback. They check:

- A single fifteen-minute budget starts before binding, survives initial and
  repeated login Intents, and retains only its remaining uptime after an early
  scheduler wakeup.
- Expiry reports `ERROR_CODE_REMOTE_EXCEPTION` once, both before registration and
  while the initial caller is already known. Authenticator-provided errors and
  package-removal cancellation keep their own codes.
- Initial waiters wake, noninteractive consumers close, response death and failed
  response IPC release state, and late results/errors cannot revive a session.
- The production service-connection callback rejects null, dead and late bindings,
  preserves authenticator IPC failures and accepts a local fixture response.
- The actual chooser wait shares the Answer deadline, closes on success,
  cancellation or interruption, and preserves interrupt status.
- Validation/delivery cannot be overtaken by timeout, and competing terminal
  errors yield one callback. A response Binder proxy is an explicit host boundary
  for death registration; no remote Binder death is claimed.

The contract follows Android 16 r4
[AccountManagerService.Session](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/accounts/AccountManagerService.java):
`TIMEOUT_DELAY_MS` is fifteen minutes, construction schedules it before binding,
`KEY_INTENT` retains that timer, `onTimedOut()` returns a remote exception and
`close()` removes the timer/death registration. The public
[AccountManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/accounts/AccountManager.java)
distinguishes this authenticator failure from a canceled Future.

Host test limits: no authenticator APK/service binding, UI login Activity, protected
credential persistence, full chooser workflow, Windows ART or cross-process Binder transport.
Connection callbacks use a local Binder and explicit synthetic authenticator operation.
The bind-failure check uses a missing host ActivityManager boundary and executes the
production exception-to-error path; it does not simulate a successful binding. Unrelated
notification descriptor reads are explicitly unsupported (ENOSYS) on Windows.

`AccountExplicitRemovalRaceHost` covers the interval after an explicit removal
has committed and released the account monitor, but before its notifications
are delivered. A JDK debugger breakpoint controls only thread scheduling; it
does not replace or instrument production bytecode. The fixture verifies the
monitor is released and the account is already removed before applying an
ordinary same-signer client update or uninstall/reinstall. An unchanged or
updated recipient receives the original account name/type once. A replacement
installation receives no old removal notification. A failed protected-store
write retains both accounts and emits no notification. The same scheduling
probe reproduces the old capture-after-unlock race using `--source-ref`.
