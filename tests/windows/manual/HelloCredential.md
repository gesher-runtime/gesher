# Verify Windows Hello through a packaged Android app

Use an interactive Windows 11 session with Windows Hello already configured.
Run the fixture with the packaged runtime and `hellocred.apk` produced by the
same exact build. The package must use the qualified AppContainer default.

```powershell
powershell.exe -NoProfile -File .\tests\windows\manual\HelloCredential.ps1 `
  -RuntimeDirectory C:\path\to\GesherPortable\App `
  -Apk C:\path\to\hellocred.apk `
  -SourceRevision <40-character-source-commit>
```

If the runtime and the temporary directory are on different drives, specify an
existing writable directory on the runtime's drive with `-ScratchDirectory`.
The script creates its own disposable profiles, disables shortcuts, and checks
each application process's actual Windows token. It leaves the user's existing
Gesher installation and profiles alone. It stops each fixture device after its
scenario. If shutdown fails, the disposable profile remains for local cleanup.

The script prints what to do before each test window opens. Focus the fixture
if needed. Enter a PIN or use a biometric only in the real Windows Hello
window. For `owner-lost`, switch to some other window after the Windows Hello
window has appeared. Leave that other window in front until the prompt closes.

| Scenario | What the script checks |
| --- | --- |
| `verify` | Android reports `DEVICE_CREDENTIAL` and `RESULT_OK` for the two real approvals. |
| `user-cancel` | Canceling both native prompts produces Android's user-canceled error and `RESULT_CANCELED`. |
| `app-cancel` | The app cancels its own prompt after five seconds. Android returns its ordinary canceled error. |
| `owner-lost` | The verifier registers loss of its owning foreground window and cancels the request. |

Each run checks that the biometric-only prompt and a Binder request carrying a
nonzero operation ID remain unavailable. The fixture does not create an
authentication-bound key. Only the Windows account credential is being
verified, which matches the approved scope in ADR 0091.

Return the commit, SHA-256 lines and PASS or FAIL lines. Report if an
expected Windows prompt never appeared. For app cancellation, confirm
whether you saw the prompt before the app canceled it. Keep PINs, biometric
details, raw app logs and screenshots private.
