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

What works, and what doesn't yet
--------------------------------
- Console programs only: Android apps (.apk) need Android's framework,
  graphics and input on top of the runtime, the next phases of the project.
- Interpreter only (no JIT and no boot image), so startup takes a few seconds.
- java.lang, collections, strings, ICU (text, locales), basic file I/O.
- Not yet: starting processes (ProcessBuilder), Selector/WatchService,
  BigInteger/crypto through BoringSSL, full time zones (no tzdata).

The runtime keeps the directories Android expects in %LOCALAPPDATA%\Gesher;
uninstalling removes them.
