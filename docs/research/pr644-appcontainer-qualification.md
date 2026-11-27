# PR 644 desktop AppContainer qualification

## Local Windows results

These checks exercised the desktop candidate with `GESHER_SANDBOX=appcontainer-trusted`
and disposable profiles. The shipping default and the user's installed copy were
unchanged. The tested runtime used the current native Windows binaries, rebuilt
ARM64 translator and app JAR, and newly generated boot images with matching core
and framework JARs.

| Route | Observed result |
| --- | --- |
| Installed APK | Launcher and guest loaded all five boot-image spaces. The current lifecycle APK reached `onResume`, looked up the USB service, verified `/data` capacity and device-state isolation, and used the real NUL device. Its native child used its Android UID and the admitted Binder connection. |
| Scoped and single-process launches | The packaged `--system-server` and `--single-process` commands loaded the five boot-image spaces and ran the lifecycle activity to `onResume`. Both exited with status 0. |
| Standalone DEX | The guest had a low-integrity AppContainer token and loaded the three core boot-image spaces. The probe verified argv, stdin, read-only input, independent file positions, denial of an adjacent file, restoration of the input ACL and exit status 23. |
| Per-app isolation override | The signed UI fixture displayed a real window. The Windows token was queried on its retained process handle. The native warning named the app and the full Windows-account access, with Cancel as the default. Cancel kept the original process alive and isolated. Approval and restoration each retired the original process and the next launch used the requested token. |
| Portable launcher and migration | Cancel in the native installation dialog returned 1223 and installed nothing. Install then Open ran the protected app. Opening an already installed version showed no installation dialog. A generated portable archive worked from a path with spaces. Moving a populated copy preserved the APK and app data and left the original profile unchanged. The candidate also refused to attach to a live unisolated device and preserved the existing APK and app data after a normal shutdown and isolated restart. |

The portable archive reused fixed suppliers from an existing build. The clean
Inno installation, Windows registration and upgrade, and the full CI shard
matrix still require a successful run.

## Roborock startup

The supplied Roborock 4.76.08 APK (`com.roborock.smart`, version code 100944)
was copied to the workspace, installed in a new profile and launched through the
installed desktop command.
The kernel reported a low-integrity AppContainer token for the process that
owned the app's window. `SplashActivity` returned a successful start result
and presented its first 400×800 frame about five seconds after invocation.
The app proceeded to `RNAccountActivity` and started its React Native bridge.

The app's installed `libpag.so` lists `libmediandk.so` as a dependency and
imports MediaCodec entry points including `AMediaCodec_createDecoderByType`.
The run translated the real ARM64 library and reached the activity without an
unresolved MediaCodec symbol or `UnsatisfiedLinkError`. This establishes the
startup path. Media playback, account sign-in, device pairing and device control
were outside this check.

On closing the window, Roborock's `Application.onTerminate` called
`Context.stopService`. Gesher had already marked the process as exiting,
and the service caller lookup rejected its application thread. The app returned
exit status 1 during cleanup. The relevant `maybeExit`, `processByPid`,
`sender` and `stopService` method bodies match main at `e2685dc71`.
This comparison is at source level. The test device shut down and removed its
device state.

The APK and its test copy had SHA-256
`f26660665dcb39b4688e976d16ff2dc270f31867780c3b1628f0263a6fa03203`.
The test used the rebuilt candidate based on integration commit `850e69880`.
Its `dalvikvm.exe` had SHA-256
`d345ee97149869368621cbf596d3205b485a454f89605ca65aed58d72fd4028b`.
The run logs and runtime hashes are under the local
`out/qa314/roborock-startup-25652/` evidence directory.

## Native and desktop follow-up checks, 2026-11-27

These are local execution results for the candidate above `3c7fe0dd2`, with the
explicit AppContainer setting and disposable profiles. They do not qualify the
shipping default, the installer or the hosted matrix.

| Check | Observed result and limit |
| --- | --- |
| Native2 directory operations | The exact x86 and ARM64 APKs downloaded from run `45946701754` reached `native2: all passed`, `onResume: all passed` and process exit 0. The held directory survives rename and replacement of its old name. The narrow relative-operation path leaves creation, complex paths, symbolic-link following and brokered storage on their established resolvers. |
| Directory negative controls | An independently rebuilt runtime with the original Bionic adapter reached the actual APK's `remove_all` assertion and reproduced `EBADF`/`SIGABRT`. A separate live guest with an advanced directory descriptor reproduced the wrong seek cookie before the fix. Independent installed probes then verified the correct first entry and advancing cookie with isolation both on and off; all 61 physical/virtual Bionic assertions passed. A failure to start an app was not counted as a negative control. |
| RTL and Windows fonts | The Windows font lookup used `/mnt/c/WINDOWS/Fonts` in a separate-volume run with the installed Arabic AppContainer app. The client-paint and window-DC captures each matched the 8,000 expected pixels per color region. Keyboard and injected Win32 mouse messages produced the expected selections. Full-content composed capture mirrored those regions in the RTL cases. Desktop screen capture was unavailable in the disconnected local session, leaving the orientation of the visibly composed client and caption unresolved. The test continues to require composed capture. |
| Media | The protected video app exited 0. Its real PCM capture contained 880.0 Hz and 660.1 Hz tones, each lasting 2.005 seconds. The capture was written inside its own CE directory and copied to the host artifact directory before uninstalling the fixture. |
| TSF | The protected app loaded the real test service from the read/execute runtime tree and completed composition and reconversion, including restricted-editor rejection. The local session had no foreground window, so only the local harness omitted that precondition. The tracked CI test retains its foreground requirement. |
| Environment diagnostics | The existing native host regression passed with the rebuilt diagnostic JNI library. A missing child retained its failing result and recorded the actual child exit status. The diagnostic does not change fixture return codes, dump the environment or establish why the hosted APK failed. |

Native source, runtime and APK hashes are retained in
`out/qa314/native-ci-repair/final-inputs.json`; that snapshot includes Bionic
SHA-256 `4b2a88eef3812a62d1b3722dcf9f1813fd9969992b82252ade062b49cb4d553f`.
The corresponding guest and host logs are in that directory. Desktop proofs are
under `out/qa314/matrix-local/`. Symlink creation was unavailable to the contained
guest on this machine; the existing resolver for that operation was preserved.

The private USB runtime USER32 initialization failure, installed CardWake and
archive RPC, clean installation/upgrade, full hosted shards, final default
launch routes and real Windows Hello consent/cancellation remain merge gates.
