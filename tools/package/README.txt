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
gesher-app starts an app's launcher Activity without a window: the app's
manifest and resources are loaded, and the Activity is created, started and
resumed, then the program exits. The bundled test app:

    gesher-app "%LOCALAPPDATA%\Programs\Gesher\lifecycle.apk"

Another Activity than the launcher one: gesher-app my.apk --activity .Other

The installer also opens .apk files with gesher-app (for your user only):
double-clicking an APK in Explorer runs it as above, in a console window.

What works, and what doesn't yet
--------------------------------
- No windows yet: Android apps run headless (gesher-app); drawing and input
  are the next phase of the project. Only apps without native (.so) code.
  The framework's natives cover what an Activity's lifecycle needs
  (resources, Looper, Parcel, in-process Binder); drawing is stubbed out and
  system services are in-process stand-ins, so apps beyond the bundled test
  app may stop at a missing native method or service.
- Interpreter only (no JIT and no boot image), so startup takes a few seconds.
- java.lang, collections, strings, ICU (text, locales), basic file I/O.
- Not yet: starting processes (ProcessBuilder), WatchService,
  BigInteger/crypto through BoringSSL, full time zones (no tzdata).

The runtime keeps the directories Android expects in %LOCALAPPDATA%\Gesher;
uninstalling removes them.
