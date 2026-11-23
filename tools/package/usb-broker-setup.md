# Optional Windows raw USB authority

The separate Gesher Raw USB Broker installer requires a Windows administrator.
Ordinary Gesher installation and updates continue to run per user.

The CI preview verifies the protected setup and service lifecycle. Package
enrollment, protected runtime launches and physical media grants are separate
integration milestones.

Install the companion from the corresponding verified Gesher build. Compare the
setup executable's SHA-256 with the digest distributed through the trusted build
or release channel before approving elevation. The companion embeds the authority
binary from that build and checks its SHA-256 before executing either service
management command.

Setup installs into `Program Files\Gesher Raw USB Broker` and registers the
`GesherUsbAuthority` Windows service for manual start. Setup checks that the
destination and the installed binary have protected ownership and write access.
It rejects an alternate destination, writable or redirected path, or a directory
containing a previous installation.

Disk selection and authorization occur separately, when requesting access to a
disk.

To remove the companion, select **Gesher Raw USB Broker** in Windows Installed
apps. Its elevated uninstaller verifies the protected binary, then asks it to
stop and unregister the service before removing the installed files. To update
the companion, uninstall the previous version before running the new setup.

## Build

Use Inno Setup 6.7 or later with the CMake target output staged from the same
verified source build as the ordinary Gesher installer.

    ISCC /DBuildDir=<verified build outputs> /DAppVersion=<version> gesher-usb-broker.iss

The input directory contains `gesher-usb-authority.exe`. The resulting setup is
named `gesher-usb-broker-setup-<version>.exe`. Keep its checksum and artifact
provenance separate from the ordinary per-user installer.
