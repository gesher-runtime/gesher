# Optional Windows raw USB authority

The separate Gesher Raw USB Broker installer requires a Windows administrator.
Ordinary Gesher installation and updates continue to run per user.

The optional companion supplies a protected Android runtime, an explicit media
selection and consent flow, and a process-bound USB block broker. It is independent
of the ordinary per-user installation. The original application APK is installed
and run; the companion does not replace the application or its Android APIs.

Install the companion from the corresponding verified Gesher build. Compare the
setup executable's SHA-256 with the digest distributed through the trusted build
or release channel before approving elevation. The companion embeds the authority
binary and the selected production runtime files from that build. Setup verifies
the SHA-256 of each installed runtime file before registering the service. Setup
and Uninstall verify the authority binary before executing it.

Setup installs into `Program Files\Gesher Raw USB Broker` and registers the
`GesherUsbAuthority` Windows service for manual start. Setup checks the
destination, runtime directories and installed files for trusted ownership,
restricted write access and reparse points. It rejects an alternate destination,
writable or redirected path, or a directory containing a previous installation.

The companion's protected `runtime` directory holds the executables, boot class
path, matching ART image, framework resources, fonts and WebView bridge. Use this
directory as the Windows working directory for the protected runtime. Under it,
`system`, `art`, `i18n` and `tzdata` supply the corresponding Android roots.
The ART image entry point is `framework/boot.art`. The authority is responsible
for separate Android data, logs and staging. The install has no user APKs or
ordinary per-user launch scripts. Its optional `filesystem-tools` subtree contains
the original Android builds of dosfstools, exfatprogs and e2fsprogs, together with
their corresponding sources, notices and build receipts. These are separate
programs loaded through the existing ELF/Bionic runtime.

## Launch and consent

After installing the companion, run the ordinary Gesher launcher's explicit
`protected-launch <absolute-apk-path>` command. The launcher invokes the fixed
installed authority through Windows elevation. The elevated dialogs identify the
APK, enumerate actual USB storage candidates, and display the selected reader's
identity and media capacity. Select read-only access or explicitly authorize
writes for that session. Cancel leaves the application without a media grant.
Use a card expressly allocated for testing before approving destructive test
operations. A selected device name alone is not write consent.

The authority retains the exact selected Windows disk object and any volume
locks across the consent handoff. It rejects internal/system media and ambiguous
volume mappings. Writes require both the user's write grant and actual writable
media. Windows or hardware protection, failed locks, removal, changed media,
expired authorization, and process termination produce errors or revoke access;
they do not become successful guest operations. A fresh card or process requires
fresh authorization. An Android USB permission decision does not by itself grant
raw media access.

The service stages the exact APK in a protected directory and keeps it pinned
against replacement. The trusted Android package verifier verifies signatures
and archive hashes. Before a suspended app resumes, the authority binds its
enrollment to the actual process handle and creation identity. A request carrying
only a PID, an environment variable, or an arbitrary executable path cannot
create a trusted Android peer. Protected enrollment currently requires a complete
single base APK; split APK enrollment fails explicitly.

Protected Android processes run as the dedicated
`NT SERVICE\GesherProtectedRuntime` identity inside AppContainers. Their Android
data, boot state, staging, logs and temporary directories are separate from the
desktop user's Android tree. Ordinary desktop processes cannot change these
trusted inputs. The installed runtime is also protected from desktop writes.
The authority and service manager register the actual `system_server` before it
runs. Its real `ApplicationSharedMemory` object is admitted only from that exact
producer and supplied to app peers with read-only access; arbitrary descriptor
passing remains rejected.

The application's ordinary `su` requests create service-authorized child
processes. Guest root operations stay within the selected media grant and the
existing Windows sandbox. The app's `/dev/block` nodes and sysfs metadata describe
the selected medium and its actual partitions. The filesystem tools operate
through the same broker on those nodes. Exit status, errors and media contents
come from the real operations. No fixture media is enabled in this runtime path.

For a disposable Windows launch test without any media grant, the installed
authority also accepts the administrator-only command
`launch-apk-no-media <absolute-apk-path>`. It starts the same protected Android
application without a raw disk or root media capability. This allows APK startup,
window rendering and APK identity checks without modifying a physical card.

To remove the companion, select **Gesher Raw USB Broker** in Windows Installed
apps. Its elevated uninstaller verifies the protected binary, then asks it to
stop and unregister the service before removing the installed files. To update
the companion, uninstall the previous version before running the new setup.

## Protected Binder startup

On a disposable test machine with an ordinary desktop user logged in, an
administrator can start the isolated Binder realm for that Windows session.
Pass the Windows session number to the service.

    sc.exe start GesherUsbAuthority binder-session <session-id>

The service launches the installed `binderd.exe` and `servicemanager.exe`
under restricted AppContainer tokens and owns the control and data endpoints.
It registers the exact service manager process as the Binder context manager
before resuming it. This starts a Binder realm without Android applications
enrolled. Stop the service to retire the realm and both processes.

## Build

Use Inno Setup 6.7 or later with the CMake target output staged from the same
verified source build as the ordinary Gesher installer.

    ISCC /DBuildDir=<verified build outputs> /DFilesystemToolsDir=<verified filesystem-tools> /DAppVersion=<version> gesher-usb-broker.iss

The input directory contains `gesher-usb-authority.exe` and the production files
listed in [usb-broker-runtime-files.iss](usb-broker-runtime-files.iss) from the
same verified installer suppliers. The allowlist also references the source
checkout's shared-library declarations and license notices. The resulting setup
is named `gesher-usb-broker-setup-<version>.exe`. Keep its checksum and artifact
provenance separate from the ordinary per-user installer.

Build the optional filesystem supplier with
[`tools/filesystem-tools/build.py`](../filesystem-tools/README.md). Its exact
source and NDK pins, source archives and full notices travel with the resulting
programs. The installer enumerates all 31 files explicitly and includes each in
its installed ownership, ACL and SHA256 checks. CI obtains this supplier from
the successful producer in the same run, source revision and attempt.

## Validation boundaries

The Windows installer check requires a disposable administrator environment. It
checks installation/uninstallation, fixed paths, SCM identity, protected ownership,
denial of ordinary desktop writes, and actual writes using the dedicated runtime
token. When an interactive desktop is available, it launches the pinned original
CardWake release and requires both a presented application frame and its original
WebView asset page. A runner without an interactive desktop reports that missing
coverage explicitly.

The isolated filesystem fixture runs the original format/check tools through the
production ELF/Bionic/POSIX code and broker protocol on private test media. It
does not validate a physical USB device. Physical validation requires a real
reader and an expressly allocated test card: reader selection, read-only denial,
approved writes and repairs, unmount, removal, replacement and revocation must be
recorded separately. A green software fixture must never be reported as a
successful hardware test.
