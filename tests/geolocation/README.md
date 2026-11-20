# Geolocation request lifetime regression

`tools/test_geolocation_retention.py` compiles the production `GeolocationShim`
against real application interfaces, then compiles it unchanged with a controlled
WebView shell and location backend. It executes real Android Handler, Looper,
MessageQueue, Location and CancellationSignal classes on a host JVM. Native queue
waits, clock/identity and trace diagnostics use explicit host boundaries. The
WebView2 boundary only records messages; the location backend provides isolated
test results and supports synchronous, asynchronous and cancellation callbacks.

The checks inspect the real MessageQueue for callbacks remaining after successful,
null, inline, timed-out, rejected or failed requests. They also cover 1,000
successful requests, 1,000 requests retired on navigation/destroy (`pageGone`),
late/duplicate responses, cancellation reentrancy and a result enqueued before
navigation. Both finite and unlimited timeouts are covered. The completion
executor runs on the WebView's own Handler rather than the application's main
executor, keeping request bookkeeping on one Looper.

The change preserves the location input, permission-denied result, timeout result
and document-generation suppression. It releases the request's callback and
cancellation state when that request ends; it does not change permission policy,
replace location input or report a successful fix when no location was returned.

Use the same JDK/framework/core/application inputs as other host Java tests:

```sh
python tools/test_geolocation_retention.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES"
```

On Windows also pass `--cc <llvm-mingw C compiler>`. The JNI fixture returns
ENOSYS for unrelated notification descriptor reads, which these tests do not use.
`--source-ref 473beec7` compiles unchanged main as a negative control: it fails
because an asynchronous completion leaves its timeout in the real MessageQueue.

This is a host lifecycle regression, not Windows ART, cross-process Binder,
live Windows location, permission-prompt, JavaScript or WebView2 validation.
