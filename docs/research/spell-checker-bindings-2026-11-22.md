# Spell-checker provider binding groups — 2026-11-22

## Scope and result

This implements the provider-group leaf of F38 and T12 in [issue #56](https://github.com/gesher-runtime/gesher/issues/56). It does not close all text-services or efficiency requirements. Baseline: `452372561e0a50c63f072a5ae1e7ca5aa81354b8`.

The previous session manager created one `IServiceConnection`, ActiveServices connection record, and provider death registration per client session. ActiveServices already shared the underlying provider `Service` and its `onBind` result. The change shares that connection ownership within one selected provider component and installed package generation, while retaining every independent session request and result.

For 1,000 concurrent sessions, actual ActiveServices connection records and provider death registrations fall from **1,000 to 1**. Both implementations still ask the real fixture provider to create and deliver **1,000 distinct sessions**. There is no fabricated spell-checker result, replacement provider, session cache, or idle group timer.

## Android contract evidence

The source is pinned to Android 16 `android-16.0.0_r4`:

- [TextServicesManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/textservices/TextServicesManagerService.java), SHA-256 `b6c666cb350da9dcd3a48a4004d7e4b08ecc9df6b8c47a19e35cc7f91e720742`: `getSpellCheckerService` and `startSpellCheckerServiceInnerLocked` reuse one group for a provider, with `BIND_AUTO_CREATE | BIND_IMPORTANT_BACKGROUND`. `SpellCheckerBindGroup` separately owns pending and ongoing requests/listeners and unbinds when the last owner leaves. `ISpellCheckerServiceCallbackBinder` holds weak references and consumes them only once.
- [SpellCheckerSession](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/view/textservice/SpellCheckerSession.java), SHA-256 `96c998324f092b02cee2f79b56e340992156f0c58b8ce804dde8bde143b3b8b3`: `close()` submits the session close before asking textservices to finish its listener ownership. Explicit finish must not send a second close to the delivered session.

The implementation uses these observable ownership rules with Gesher's existing serialized worker and actual privileged ActiveServices binding path. Public caller validation, user-0 restriction, provider visibility and selection, `BIND_TEXT_SERVICE` discovery, component enabled state, and binding flags are unchanged. Gesher's existing provider-death/null-binding retirement and duplicate-listener policies remain unchanged; this leaf does not claim complete Android provider-reconnection behavior.

## Ownership and retirement

`SpellCheckerSessions` owns a component-keyed group only while it has live or pending clients. Each request still has its own locale, copied extras, supported attributes, listener, ready callback, owning PID and eventual session. Requests may complete and finish in a different order from admission.

The group owns one actual service connection and provider death link. Finishing or losing one client removes only its request and client death link. A provider transport failure retires only its own group; a request-local runtime error or failed ready callback retires only that request. The final owner removes the group before unbinding, so queued callbacks cannot act on a successor group with the same component.

A static, one-shot creation callback holds weak references to the manager and request. A provider retaining a completed or canceled callback cannot keep the first client alive while another client owns the shared group. A first result that arrives after request retirement is closed; repeated invocations of the same consumed callback are ignored, including a duplicate of an already-delivered session.

Package removal captures the actual current `ApkPackage` before enqueueing cleanup, using a narrow `SpellCheckerCatalog.owner()` accessor. Queued cleanup compares object identity, so it cannot retire a later installation with the same package and component names. Admission, dispatch and completion retain current-generation/enabled-state checks. The manager still creates no session when the provider returns no session.

## Measured work

The same fixture compiles the actual historical or candidate `SpellCheckerSessions` and `SpellCheckerCatalog` sources over the same complete app/framework dependencies. It calls the actual session manager, ActivityManagerService and ActiveServices. It does not replace `bindService` or synthesize connection counts.

Four fresh JVMs ran in candidate/baseline/baseline/candidate order, all with `--case work`. Each process uses the same 1/10/100/1,000-session sequence, followed by 1,000 sequential opens and closes while another client retains the binding. Selected manifest metadata and client binders are constructed before the timed interval. The provider's independent creation and delivery callbacks execute inside it. Caller and actual worker thread allocation/CPU counters include fixture orchestration and production controller work; they are component measurements, not whole-app speedups.

| Observation | Baseline | Candidate |
| --- | ---: | ---: |
| Concurrent sessions delivered | 1,000 | 1,000 |
| Actual concurrent connection records | 1,000 | 1 |
| Provider death links for concurrent sessions | 1,000 | 1 |
| Underlying service instances / `onBind` calls | 1 / 1 | 1 / 1 |
| Concurrent allocated bytes, median | 7,486,576 | 1,686,856 |
| Concurrent caller + worker CPU, median | 110.80 ms | 31.32 ms |
| Repeated-session provider death links, including anchor | 1,001 | 1 |
| Repeated-session provider death unlinks while anchor lives | 1,000 | 0 |
| Repeated-session peak connections | 2 | 1 |
| Repeated-session allocated bytes, median | 9,136,868 | 3,347,100 |
| Repeated-session caller + worker CPU, median | 196.15 ms | 179.25 ms |

Concurrent allocations decrease **77.47%** and measured caller/worker CPU **71.73%** in this workload. Repeated-session allocations decrease **63.37%**; repeated-session CPU varied materially across passes. An earlier pass with identical work methods measured medians of 199.53 ms baseline and 197.04 ms candidate, versus the final values above. Both passes are preserved in the receipt; the repeated-session improvement claim is allocation and connection/death ownership, without a CPU speedup claim. The first one-session case also includes cold class initialization and is retained in the raw data without a cold-start improvement claim.

The maintained gate enforces observed connection/death-link bounds and complete independent delivery, rather than a timing threshold. Applying the candidate bound to unchanged baseline sources fails at 10 sessions with `expected 1, observed 10`, demonstrating that the gate detects the actual old resource behavior. The [JSON receipt](spell-checker-bindings-2026-11-22.json) contains every run, command, source/dependency hash, log hash and measurement, including the baseline values.

## Validation and CI binding

- Full app Java and DEX build against the real Android framework/core class artifacts passed. Compatibility metadata generation found 16 changes with zero unresolved source definitions.
- Final full services Java/DEX build passed, including the target Parcel mode of the new fixture. All 105 final contracts also pass on a host interpreter (`-Xint`); the weak-retention assertion uses `Reference.refersTo(null)` so the assertion itself never materializes a strong interpreter root.
- The maintained `tools/test_service_rebinding.py` default entry point passed its existing **431** contracts, then invoked the new gate and passed **105** session ownership contracts and both resource workloads.
- Contracts cover independent locales/extras/attributes and callbacks, out-of-order completion, cancel/finish, PID ownership, client/provider death, null binding and null creation result, provider/link/callback failures, partial binding publication failure, reentrant finish, concurrent admission/finish, listener reuse, delayed old connection/death callbacks, package replacement and removal, weak callback retention, and final unbinding without an idle timer.
- An independent agent inspected the actual production paths against the pinned Android source and independently replayed the maintained entry: **431 + 102** contracts and both workloads. It then inspected the null-result isolation addition and independently replayed all **105** session contracts. A final fixture-only refinement uses `refersTo(null)` for the three weak-retention observations; its measured work methods and production are unchanged. Its source verdict and immutable commit binding are recorded with the publication review.

The existing pinned framework recipe `972c2b42b40c4ac799e147f5e352d07ba615d0fb` already invokes the candidate `tools/test_service_rebinding.py`. The new host gate is reached through that maintained entry point with the same framework/core/app/apksig/JDK inputs; no unexecuted branch-only workflow step or recipe-pin change is needed. `TextServicesContractTest` also invokes `SpellCheckerBindingContractTest.contracts(true)` before its success marker in the existing Windows track 99.

The host explicitly constructs already-selected manifest metadata, provider/application-thread endpoints and death delivery. The JNI fixture supplies Android platform identity/clock boundaries, the host sets the property-cache test namespace and a locale-format setting, and provider discovery/real APK installation are outside that host workload. Target mode uses actual generated Binder interfaces and native Parcel construction, but **Windows/ART, kernel-delivered Binder death, and real installed-provider APK execution were not run locally**. Compiling and wiring that target gate does not constitute a Windows runtime pass.

## Reproduction

With paths to the actual Android class outputs and JDK:

```sh
python3 tools/test_service_rebinding.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" --apksig "$APKSIG"

# Identical work-only baseline/candidate process; omit --source-ref for candidate.
python3 tools/test_spell_checker_bindings.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" --apksig "$APKSIG" \
  --case work --source-ref 452372561e0a50c63f072a5ae1e7ca5aa81354b8

# Add --require-shared to the historical command for the expected-failure control.
```
