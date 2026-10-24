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

Running an Android app (gesher-app)
-----------------------------------
Android's framework (framework.jar, framework-res.apk) comes with Gesher.
gesher-app installs an app and starts its launcher Activity, as on Android:
on a "Gesher device" of your own, a system server (with binderd and
servicemanager) that every launch shares and that starts each app in a
process of its own. The Activity shows in a window of its own (drawn by
Android's renderer, on the CPU); closing the window finishes the Activity,
and the program ends with the app's process. The app's output shows in the
console. The bundled test app:

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

Each installed app gets a Start menu entry, in the "Gesher Apps" folder, with
its name and icon; it starts the app as

    gesher-app launch com.example.app

Uninstalling the app removes its entry; uninstalling Gesher removes the
folder.

Other options: --activity .Other (another Activity), --ez KEY true|false
(an extra in the intent), --headless (no window: once the Activity is idle
the launcher closes it; with --keep-task the app runs until it finishes;
GESHER_APP_WINDOW=headless makes it the default), --hidden-window (the
window is drawn off the desktop).
The logs are in %LOCALAPPDATA%\Gesher\logs (<package>.log for an app).

Without the device, the app alone in one process with stand-in services:

    gesher-app my.apk --single-process

Double-clicking an .apk file runs it with gesher-app too, unless another
program already opens .apk files; then use "Open with" (the Gesher entry
runs gesher-app.cmd).

What works, and what doesn't yet
--------------------------------
- Windows show what the app draws, but take no input yet (clicks and
  keys are the next step); every window is a top-level window.
  The framework's natives cover what an Activity's lifecycle and its views
  need (resources, Looper, Parcel, Binder, android.graphics); the system
  services are Gesher's own, partly stand-ins, so apps beyond the bundled
  test app may stop at a missing native method or service.
- Interpreter only (no JIT and no boot image), so startup takes a few seconds.
- java.lang, collections, strings, ICU (text, locales), basic file I/O.
- Not yet: starting processes (ProcessBuilder), WatchService,
  BigInteger/crypto through BoringSSL, full time zones (no tzdata).

The runtime keeps the directories Android expects in %LOCALAPPDATA%\Gesher;
uninstalling removes them.
