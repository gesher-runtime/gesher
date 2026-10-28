# Exec4: remote property report proof

## Scope and source

This bounded slice exercises the existing Binder property report path with two
real ART processes. It adds no property mutation watcher and no new product
API. Ordinary property writes must remain silent until an explicit report or
Binder poke, as ADR0060 requires.

Pinned android-16.0.0_r4 sources establish the contract:

- [SystemPropPoker](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/packages/SettingsLib/src/com/android/settingslib/development/SystemPropPoker.java)
  sends an empty Parcel with SYSPROPS_TRANSACTION to each selected service;
  it ignores the transaction's boolean return.
- [JavaBBinder](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/jni/android_util_Binder.cpp)
  always calls native BBinder::onTransact for that code after Java dispatch.
- [BBinder](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/binder/Binder.cpp)
  calls binder::os::report_sysprop_change. Gesher already builds OS_android.cpp,
  which forwards to the existing native callback registry.
- [SystemProperties JNI](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/jni/android_os_SystemProperties.cpp)
  calls Java callbacks only on a VM-attached thread. This fixture uses the real
  JavaBBinder dispatch thread; no unattached native watcher is introduced.

BinderTest retains its original proxy/PID, nested callback, listing, death and
dead-service assertions. Its owned service now registers a Java property
callback and exposes count/value snapshots through separate transactions. The
client proves ordinary writes produce zero reports; a local explicit report
stays local; two actual remote pokes produce exactly one report each and read
the corresponding live value; removal prevents a third poke from invoking the
callback. The original service-exit/death checks then verify its lifecycle.
Raw count/value/PID snapshots and actual poke return values are published in
both Windows job logs, and the existing harness requires the added verdict.

This is proof of the existing explicit per-recipient report path only. The
same-Windows-account area is not an Android security boundary. Authenticated
property caller policy and privileged all-service fanout remain open and depend
on the Binder/services identity work. No aggregate roadmap box is checked.

## Status and next steps

Base f1550540186b4ae3f17a5e501a886ec1c4d5d613, branch
wip/exec-property-policy. Integration peer approved the architecture before
editing. Local checks are source inspection, diff whitespace and PowerShell
parsing only; all Java/native compilation and fixtures remain CI-only.
Independent guest_ipc_diagnosis source peer reviewed exact source
a1e06af6c14d026c02995c1ecf0dfdb84b6090c3 and passed the bounded assertions.
Canonical full CI40784262363/attempt1 was dispatched at that source. It remains
pending; no runtime acceptance is claimed. Watch output is retained at
C:\work\gesher-ci-review\exec4-property-reports-watch-40784262363.log.
Root requested central watch ownership: only local gh watch process8008
(session16637) was stopped; the remote CI was not cancelled. Integration must
reattach with interval120, audit actual compile/archive receipts and both
Windows raw report-state records, original whole-suite verdicts, package and
both e2e logs. Current head after this source differs only by documentation.
