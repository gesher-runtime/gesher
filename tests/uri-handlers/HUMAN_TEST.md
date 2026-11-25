# Human test: Windows mailto, tel, sms and geo handlers

From the PR's HUMAN_TEST.md, section A. Run on Windows 10/11 from a checkout of this branch.

## 1. Windows host test (x64 Native Tools Command Prompt for VS 2022)

```
cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 compat\devsvc\url_handlers_win.cc tests\uri-handlers\url_handlers_windows_test.cc /Fe:url-handlers-test.exe shlwapi.lib shell32.lib advapi32.lib
url-handlers-test.exe
```

Expected (the `info` lines depend on the machine; everything else exactly):

```
ok   empty:  is invalid scheme
ok   leading digit: 1tel is invalid scheme
ok   space: mail to is invalid scheme
ok   colon: mailto: is invalid scheme
ok   path: ..\mailto is invalid scheme
ok   too long: aaaa...a is invalid scheme
ok   unregistered scheme: gesher-test-<pid>-cmd is not registered
ok   per-user protocol with an open command: gesher-test-<pid>-cmd is registered
ok   "URL Protocol" without a handler: gesher-test-<pid>-bare is not registered
ok   after removal: gesher-test-<pid>-cmd is not registered
info mailto: registered (hr 0x00000001)
info tel: registered (hr ...)          <- "registered" if Phone Link (or another app) handles tel:
info sms: ...
info geo: ...
info http: registered (hr 0x00000001)
url-handlers PASSED: 11 checks, 0 failures
```

Exit code 0. Nothing opens. The test writes and removes
`HKCU\Software\Classes\gesher-test-<pid>-cmd` and `-bare` only.

## 2. Probe: what Windows reports per scheme

```
cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DUNICODE /D_UNICODE compat\devsvc\url_handlers_win.cc tests\uri-handlers\url_handler_probe.cc /Fe:url-handler-probe.exe shlwapi.lib shell32.lib user32.lib advapi32.lib
url-handler-probe.exe
```

Expected: one block per scheme (`mailto`, `tel`, `sms`, `smsto`, `geo`, `http`), e.g.

```
mailto: registered
    app: Outlook          (or Mail, Thunderbird, ...)
    AppUserModelID: ...
    command: ...          (or "(none, hr ...)" for a packaged app)
    DelegateExecute: ...  (a CLSID for a packaged app)
```

Please paste the full output into the PR conversation. Two things to look for:

- Each scheme that shows an `app:` must say `registered`, and each that says
  `not registered` must have neither a command nor a DelegateExecute. A scheme that
  shows an app but `not registered` is a bug (the lookup misses that kind of handler).
- `smsto` is expected to be `not registered` on most machines; Gesher asks Windows
  about `sms` for both `sms:` and `smsto:`.

Then launch each registered scheme exactly as Gesher will:

```
url-handler-probe.exe --open "mailto:a@example.com?cc=b@example.com&subject=Gesher%20test&body=line%201%0D%0A%D7%A9%D7%9C%D7%95%D7%9D"
url-handler-probe.exe --open "tel:+972501234567"
url-handler-probe.exe --open "sms:+972501234567?body=%D7%A9%D7%9C%D7%95%D7%9D"
```

Expected:
- mailto: a new message in the default mail app, To `a@example.com`, Cc
  `b@example.com`, subject `Gesher test`, body two lines, the second `שלום`.
- tel: (only if registered) **record whether the handler shows the number and waits
  for you to press Call, or places the call immediately.** Android's DIAL requires
  the first. If it calls immediately, say so in the PR: the DIAL activity must then be
  withdrawn.
- sms: (only if registered) a new message to the number with body `שלום`, not sent.

## 3. ART services test (needs the full Gesher build)

Build in WSL as in README.md "Building":

```
tools/fetch-aosp.sh
python3 tools/toolchain/prepare_pthread_cancel.py build
export GESHER_HOST_PTHREAD_SELECTION=publisher-cancel
export GESHER_PTHREAD_CANCEL_RECEIPT="$PWD/out-pthread-cancel/pthread-cancel-receipt.json"
export GESHER_PTHREAD_CANCEL_RECEIPT_SHA256="$(sha256sum "$GESHER_PTHREAD_CANCEL_RECEIPT" | cut -d' ' -f1)"
gesher_llvm="$PWD/out-pthread-cancel/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
cmake --fresh -S . -B out -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake \
  -DLLVM_MINGW="$gesher_llvm" -DCMAKE_PROJECT_INCLUDE="$PWD/tools/toolchain/pthread_cancel_link.cmake"
ninja -C out
tests/build-test-apps.sh out/test-apps
```

Then on Windows, in PowerShell from the checkout:

```
tests/windows/run-tests.ps1 -Dir <path to out> -Shard services
```

Expected among the results: `services: mailto/tel/sms/geo resolve and open only through
a registered Windows handler; otherwise no activity` passes, and its output has one
line per scheme, for example

```
UriHandlerIntentTest: mailto Windows handler registered: resolves and opens mailto:a@example.com?subject=Gesher%20test
UriHandlerIntentTest: tel Windows handler registered: resolves and opens tel:+15550100
UriHandlerIntentTest: sms Windows handler not registered: no activity, START_INTENT_NOT_RESOLVED
UriHandlerIntentTest: geo Windows handler not registered: no activity, START_INTENT_NOT_RESOLVED
UriHandlerIntentTest: CALL, mms and SEND message/rfc822 have no Windows handler activity
UriHandlerIntentTest all passed
```

The registered/not registered words must match step A2's probe. Nothing opens (the
test replaces the opener). The `services: a browser for apps...` check must still pass.

## 4. End to end with a real app (not yet run by the author)

Install the Gesher built in A3 (`out/installer/gesher-setup-*.exe`, or the package
job's installer) and an app with a "Contact us" / "Send feedback" e-mail action and,
if available, a "Call" or "Text us" link (for example a store, bank or delivery app).

1. Tap the e-mail action. Expected: the Windows mail app opens a new message with the
   app's address, subject and body. The system log (`%LOCALAPPDATA%\Gesher\logs`) has
   `startActivity: Windows external URL opened (mailto)`.
2. Tap a phone link. Expected: as in A2 for `tel:` when registered; when Windows has no
   `tel:` handler, the app shows its own "no app" message (it received
   `ActivityNotFoundException`) and nothing opens.
3. Temporarily remove the default for `mailto` is not possible in Windows 11 settings;
   to see the "no handler" path instead, use a scheme the machine has no handler for
   (A2 shows which, typically `geo`): an app's "Show on map" button must report "no
   app", not open anything.
