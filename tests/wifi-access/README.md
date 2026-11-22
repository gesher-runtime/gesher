# Wi-Fi caller and callback host contracts

`tools/test_wifi_access.py` compiles the production `WifiAccess`, `WifiManager`,
`ConnectivityManager` and `IWindowsWifi` sources, using the built application
classes and Android framework/core classes. It runs their real Java methods,
framework value objects and connectivity callback registry.

The isolated boundaries are `Context` permission answers, a local Wi-Fi Binder
service with canned replies for two interfaces, deterministic Handler dispatch,
and JNI initialization/clocks. None of these fixtures is available to ordinary
apps. The production Wi-Fi service and native backend have separate harnesses.

Coverage includes missing and revoked basic permissions, preservation of the
service's redacted results even with forged client permission answers,
Connectivity callback opt-in, per-registration context capture, fresh service
checks for queued events, no-context fail-closed behavior, exact interface
identity, legacy and sticky-broadcast SSID privacy, client identity/attribution
forwarding, and service unavailability/error propagation. Location and scan
authorization belong to `WifiService`; the caller facade must delegate them and
avoid duplicate AppOps usage notes. This fixture does not imitate that service's
permission policy or claim to verify it.

Run with the same Java 21, framework/core inputs and application classes as the
app build:

```sh
python tools/test_wifi_access.py \
  --java-home /path/to/jdk \
  --framework-classes /path/to/framework-classes \
  --core-classes /path/to/core-classes \
  --app-classes /path/to/gesher-app-classes.jar
```

On Windows, pass `--cc /path/to/llvm-mingw/bin/clang.exe` when the C compiler is
not on `PATH`. The optional `--case` selects `helper`, `wifi`, `wifi-no-access`
or `connectivity`. For a negative control, `--source-ref` replaces only the two
manager source files with that Git revision while retaining the current helper
and fixtures, compiling the real backend dependency used by the old managers.
The pre-repair managers must fail the `wifi-no-access`
and `connectivity` cases.

Passing this host harness does not establish Binder Parcel interoperability,
actual Windows WLAN access or Android/ART execution. Service security and native
WLAN tests cover their own declared boundaries; Windows/ART and physical-device
checks require separate evidence.
