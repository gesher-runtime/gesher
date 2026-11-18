# Account session lifetime regression

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
its schedule call while the completion thread starts. Cleanup must leave the real
timer queue empty and clear listeners, results and pending bundles. The baseline
executor is unwrapped reflectively so the negative control inspects its actual
queue instead of merely testing the executor's type.

Host test limits: no authenticator APK/service binding, UI login Activity, protected
credential persistence, Windows ART or cross-process Binder transport. Unrelated
notification descriptor reads are explicitly unsupported (ENOSYS) on Windows.
