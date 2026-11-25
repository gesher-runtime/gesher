# Persisted logs and credential redaction

On Android, logcat is an in-memory ring buffer in `logd`. An application reads
only its own records, and other records need the privileged `READ_LOGS`
permission ([Logcat](https://developer.android.com/tools/logcat),
[`READ_LOGS`](https://developer.android.com/reference/android/Manifest.permission#READ_LOGS)).
The buffer does not survive a reboot. An app process's stdout and stderr go to
logcat (`System.out`/`System.err`) or to `/dev/null`.

Gesher has no `logd`. It keeps each process's log in files in the profile's
logs directory (`%LOCALAPPDATA%\Gesher\logs` by default, `$GESHER_LOGS` to
override; see `Device.logsDir`):

| File | Written by |
| --- | --- |
| `<package>.log.logcat` | Binary `GOLG` record stream (`GESHER_LOG_FILE`, `compat/posix/src/logging.cc`). `logcat` reads it. |
| `<package>.log` | The app process's stdout and stderr: liblog's stderr echo (Java `Log`, host liblog's forwarded logger, the guest `__android_log_*` in `compat/bionic/stdio_bionic.cc`) and `System.out`/`System.err`. |
| `system_server.log`, `binderd.log`, `servicemanager.log` | The device's own processes' stdout and stderr. |
| `<package>.crash.log` | App crash notice reports (`CrashReport`, [app-crash-notices.md](app-crash-notices.md)). |

These files last longer than logcat and are easier to collect: backups,
support requests and the Control Center's "Open logs location" all reach them.
Applications and libraries log credentials at debug level because they expect
logcat's limits. microG, for example, logs Google OAuth access tokens
(`getAuthToken: ya29.…`) and the GCM `auth_token`. Gesher therefore replaces
credential values with `<redacted>` **before** they are written to any of these
files. This applies to every package and every priority.

## What is redacted

Values are replaced and the surrounding text is kept, so the log stays useful:

- Google token formats, wherever they appear: `ya29.` access tokens,
  `aas_et/` master tokens, `oauth2_4/` codes, and `1//` refresh tokens (at least
  20 token characters, and not after a letter or digit, because `1//` also
  appears in ordinary text). The prefix is kept (`ya29.<redacted>`).
- `Bearer <token>`.
- `key=value`, `key: value`, `"key": "value"` and `key='value'` whose key ends
  with `token`, `secret`, `password`, `passwd` or `cookie`, or is
  `authorization`, `auth`, `apikey` or `api_key`, ignoring case. This covers
  `auth_token`, `authToken`, `securityToken`, `access_token`, `refresh_token`,
  `id_token`, `Token`, `client_secret`, `Passwd`, `Cookie`, `Set-Cookie` and
  `Authorization`. An unquoted value ends at whitespace or at one of
  `, ; & ) } ] " '`. A quoted value ends at its closing quote. `Authorization`
  and `…cookie` header values run to the end of the line (`Bearer x`,
  `a=1; b=2`).

Matching uses ASCII only. Other bytes, such as UTF-8 text, pass through
unchanged, and a value is never cut inside a multi-byte character. Redacting
text that is already redacted changes nothing.

## Where it happens

The rules are implemented twice, with the same tests
(`tests/logging/redaction_vectors.inc`):

- Native: `gesher::logging::RedactCredentials`. `Record` applies it before
  framing a `GOLG` record, so the length fields describe the redacted text. A
  message that grows past the 4096-byte field is cut at the field's limit, as
  any long message is. The host liblog logger
  (`compat/posix/src/android_log_write.cc`) gives the forwarded stderr logger
  the redacted message. The guest liblog (`LogWrite` in
  `compat/bionic/stdio_bionic.cc`) redacts before both sinks. The format
  arguments of `__android_log_print` are joined before redaction, so a token
  split across them is still found. Scanning is one pass with no regular
  expressions. A message with no credential allocates nothing, and the
  record's lock-free fast path is unchanged.
- Java: `com.gesher.app.LogRedaction`. `GesherAppProcess`, `Main` and
  `SystemServer` install it on `System.out`/`System.err` at startup. It
  redacts each line before writing it. A complete line is written at once. A
  partial line waits for its newline, an explicit flush, exit or 8 KiB, so
  `print("ya2"); print("9.…")` is still recognized. `CrashReport` redacts the
  stack trace text.

Records are redacted when they are written, so `logcat` (Gesher's reader of
the persisted buffer) shows the redacted text, including to the app that
logged it. Log APIs, tags, priorities, loggability properties and record
framing are unchanged.

## Logs from earlier versions

When the device starts, `Launcher` rewrites the logs directory once, before any
of the device's processes run (`LogRedaction.scrubLogs`):

- Text logs are streamed through the same redaction into a temporary file. The
  temporary file replaces the log only when something changed.
- A `.logcat` file is rewritten record by record with corrected lengths. A file
  that is not a valid record stream is deleted, because `logcat` cannot read it
  anyway.

The marker file `.credentials-redacted-v1` records the pass. If a file cannot
be replaced (for example, a process left from an older device still holds it),
the marker is not written and the pass runs again at the next start.

## Limits

- Bytes a process writes straight to its stdout/stderr descriptors from
  native code (`write(2, …)`, `printf`), other than through liblog or Java's
  `System.out`/`System.err`, are not filtered.
- Credentials in formats other than those above are not recognized. Examples
  are an unlabeled opaque session ID and a key name that is not on the list
  (`X-Goog-Api-Key`).
- A credential split by an explicit flush in the middle of a Java line, or by
  a native stderr write in the middle of one, is redacted only in the part
  that still matches.
