Gesher Portable
===============

Extract the complete GesherPortable folder, then open Gesher.exe to open the
Control Center. Keep Gesher.exe, portable.marker, App and Data together.
You can drag an APK onto Gesher.exe to install it: a window asks first
(Install or Cancel), nothing is installed until you click Install, and the
app starts only when you click Open afterwards. If the same version is
already installed, the app simply opens.

App contains the runtime. Data contains your installed Android apps, their data,
settings and logs. Close all Android apps and shut down the Gesher device before
moving the folder or safely ejecting a USB drive. Keep a backup of Data.

Android apps run with your Windows user's permissions; this is not a security
sandbox. Read security-notice.txt (or security-notice.he.txt) before installing
apps. Only install apps you trust.

WebView features need the Microsoft Edge WebView2 Runtime on the host computer.
The portable package does not install system prerequisites. Hardware features
depend on the computer and on the permissions you grant.

Portable mode does not add app shortcuts to the Windows Start menu or the
desktop, change file
associations or register app URL protocols. Internal Android intents still work.
Sign-in flows that return from an external browser through a custom URL scheme
need that scheme to be registered on the host; portable mode does not add it.
Windows notifications can still create per-user AppUserModelID metadata. Windows
and the apps can leave other host records, so portable mode is not a guarantee
that the computer will retain no traces.

The Control Center updater keeps the current portable runtime directory and Data.
To update manually with Setup, select portable mode and choose the existing App
directory (or the runtime directory of a flat portable installation). Back up Data
first. Never install the standard mode over a portable runtime directory.
