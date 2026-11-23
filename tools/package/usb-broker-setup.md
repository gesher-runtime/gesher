# Optional Windows raw USB authority

The separate Gesher Raw USB Broker installer requires a Windows administrator.
Ordinary Gesher installation and updates continue to run per user.

The CI preview packages the Windows Android runtime with the protected setup and
checks the service lifecycle. Package enrollment, protected runtime launches and
physical media grants are separate integration milestones.

Install the companion from the corresponding verified Gesher build. Compare the
setup executable's SHA-256 with the digest distributed through the trusted build
or release channel before approving elevation. The companion embeds the authority
binary and the selected production runtime files from that build. Setup verifies
the SHA-256 of each installed runtime file before registering the service. Setup
and Uninstall verify the authority binary before executing it.

Setup installs into `Program Files\Gesher Raw USB Broker` and registers the
`GesherUsbAuthority` Windows service for manual start. Setup checks that the
destination, runtime directories and installed files for trusted ownership,
restricted write access and reparse points. It rejects an alternate destination,
writable or redirected path, or a directory containing a previous installation.

The companion's protected `runtime` directory holds the executables, boot class
path, matching ART image, framework resources, fonts and WebView bridge. Use this
directory as the Windows working directory for the protected runtime. Under it,
`system`, `art`, `i18n` and `tzdata` supply the corresponding Android roots.
The ART image entry point is `framework/boot.art`. The authority is responsible
for separate Android data, logs and staging. The install has no user APKs or
ordinary per-user launch scripts.

Disk selection and authorization occur separately, when requesting access to a
disk.

To remove the companion, select **Gesher Raw USB Broker** in Windows Installed
apps. Its elevated uninstaller verifies the protected binary, then asks it to
stop and unregister the service before removing the installed files. To update
the companion, uninstall the previous version before running the new setup.

## Protected Binder startup

On a disposable test machine with an ordinary desktop user logged in, an
administrator can start the isolated Binder realm for that Windows session.
Pass the Windows session number to the service.

    sc.exe start GesherUsbAuthority binder-session <session-id>

The service launches the installed `binderd.exe` under a restricted
AppContainer token and owns the control and data endpoints. The protected
realm starts with no Android applications enrolled. Stop the service to
retire that realm and its process.

## Build

Use Inno Setup 6.7 or later with the CMake target output staged from the same
verified source build as the ordinary Gesher installer.

    ISCC /DBuildDir=<verified build outputs> /DAppVersion=<version> gesher-usb-broker.iss

The input directory contains `gesher-usb-authority.exe` and the production files
listed in [usb-broker-runtime-files.iss](usb-broker-runtime-files.iss) from the
same verified installer suppliers. The allowlist also references the source
checkout's shared-library declarations and license notices. The resulting setup
is named `gesher-usb-broker-setup-<version>.exe`. Keep its checksum and artifact
provenance separate from the ordinary per-user installer.
