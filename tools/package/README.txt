Gesher: ART (Android's runtime) on Windows - a trial build
==========================================================

Requirements: Windows 10 or 11, x64. Nothing else needs installing.

Running Hello
-------------
From the Start menu, open "Gesher Hello". Or, in a new command prompt:

    gesher

The expected output, after a few warnings about the missing boot image:

    Hello from ART on Windows

Running your own program
------------------------
1. Compile your Java code (JDK 17 or 21) and pack it into a jar:
       javac -d classes Main.java
       jar cf app.jar -C classes .
2. Convert it to dex with d8 (from Android's build-tools); this writes
   classes.dex:
       d8 --min-api 36 --output . app.jar
3. Run it:
       gesher classes.dex Main
   or, with arguments:
       gesher classes.dex com.example.Main arg1 arg2

The .dex file must be on the same drive as Gesher (usually C:).

Running an Android app, headless (gesher-app)
---------------------------------------------
Android's framework (framework.jar, framework-res.apk) comes with Gesher.
gesher-app installs an app and starts its launcher Activity without a window,
as on Android: on a "Gesher device" of your own, a system server (with
binderd and servicemanager) that every launch shares and that starts each
app in a process of its own. The app's output shows in the console; once the
Activity is resumed and idle, the launcher closes it (no window yet) and the
program ends with the app's process. The bundled test app:

    gesher-app "%LOCALAPPDATA%\Programs\Gesher\lifecycle.apk"

The first launch starts the device in the background (no window; it outlives
the console) and later launches use it. It stops by itself after 10 minutes
without a running app (GESHER_IDLE_TIMEOUT, in seconds), or with:

    gesher-app shutdown

Installed apps stay installed (%LOCALAPPDATA%\Gesher\data\app):

    gesher-app install my.apk
    gesher-app list
    gesher-app uninstall com.example.app
    gesher-app status

Other options: --activity .Other (another Activity), --keep-task (let the
app run until it finishes), --ez KEY true|false (an extra in the intent).
The logs are in %LOCALAPPDATA%\Gesher\logs (<package>.log for an app).

Without the device, the app alone in one process with stand-in services:

    gesher-app my.apk --single-process

Double-clicking an .apk file runs it with gesher-app too, unless another
program already opens .apk files; then use "Open with" (the Gesher entry
runs gesher-app.cmd).

What works, and what doesn't yet
--------------------------------
- No windows yet: Android apps run headless (gesher-app); drawing and input
  are the next phase of the project. Only apps without native (.so) code.
  The framework's natives cover what an Activity's lifecycle needs
  (resources, Looper, Parcel, Binder); drawing is stubbed out and the system
  services are Gesher's own, partly stand-ins, so apps beyond the bundled
  test app may stop at a missing native method or service. Apps whose theme
  has an action bar need fonts, which are not installed yet: try
  --single-process for them.
- Interpreter only (no JIT and no boot image), so startup takes a few seconds.
- java.lang, collections, strings, ICU (text, locales), basic file I/O.
- Not yet: starting processes (ProcessBuilder), WatchService,
  BigInteger/crypto through BoringSSL, full time zones (no tzdata).

The runtime keeps the directories Android expects in %LOCALAPPDATA%\Gesher;
uninstalling removes them.
