# Device identity

What `android.os.Build` and the device name tell applications about the device
Gesher runs on. Maintainer decision (2026-11-25, recorded in
[CONTRIBUTING.md](../CONTRIBUTING.md)'s examples): Gesher reports the PC's real
manufacturer and model, its own brand, product and fingerprint, and the Windows
computer name as the device name. It does not impersonate another device.

## What applications see

| Field | Property | Value |
| --- | --- | --- |
| `Build.MANUFACTURER` | `ro.product.manufacturer` | The PC's manufacturer (SMBIOS), for example `Dell Inc.`, `LENOVO`, `Microsoft Corporation` |
| `Build.MODEL` | `ro.product.model` | The PC's model (SMBIOS), for example `XPS 13 9310`, `ThinkPad X1 Carbon 5th`, `Virtual Machine` |
| `Build.BRAND` | `ro.product.brand` | `gesher` |
| `Build.PRODUCT` | `ro.product.name` | `gesher` |
| `Build.DEVICE` | `ro.product.device` | `gesher` |
| `Build.BOARD` | `ro.product.board` | `gesher` |
| `Build.HARDWARE` | `ro.hardware` | `gesher` |
| `Build.BOOTLOADER` | `ro.bootloader` | `gesher` |
| `Build.HOST` | `ro.build.host` | `gesher` |
| `Build.FINGERPRINT` | `ro.build.fingerprint` | `gesher/gesher/gesher:16/BP4A.251205.006/11252199:user/release-keys` |
| `Build.ID`, `Build.VERSION.*`, `Build.TYPE`, `Build.TAGS` | `ro.build.*` | android-16.0.0_r4's (`BP4A.251205.006`, SDK 36, security patch 2025-12-05), unchanged |
| `Settings.Global.DEVICE_NAME` | | The Windows computer name, unless the user set another |
| `BluetoothAdapter.getName()` | | The computer name, as Windows advertises it ([bluetooth.md](bluetooth.md)) |

The fingerprint follows Android's format,
`$(BRAND)/$(PRODUCT)/$(DEVICE):$(VERSION.RELEASE)/$(ID)/$(VERSION.INCREMENTAL):$(TYPE)/$(TAGS)`,
made of exactly the fields above (`Build.java` derives the same string when
`ro.build.fingerprint` is unset). `ro.build.flavor` is `gesher-user`.

Google Play's web site lists a device by `Build.MANUFACTURER` and `Build.MODEL`
(microG's check-in reads `android.os.Build`), so it shows the PC, for example
"Dell Inc. XPS 13 9310". The WebView user agent carries `Build.MODEL`
as Android WebView's does.

## Manufacturer and model

When a boot's property area is created (`Initial()` in
`compat/posix/src/system_properties.cc`, the one place every process's
properties start from: dalvikvm, gesher-app, ELF bionic, `getprop`), Gesher reads
the firmware's SMBIOS table with `GetSystemFirmwareTable('RSMB')` (kernel32,
documented, no WMI) and parses it in `compat/posix/include/gesher/device_identity.h`:

- `Build.MANUFACTURER`: the System Information (type 1) manufacturer; when that
  is a placeholder, the Baseboard (type 2) manufacturer; when both are,
  `Gesher`.
- `Build.MODEL`: the type 1 product name; when that is a placeholder, the type 1
  family, then the baseboard product name; when all are, `PC`. Lenovo puts the
  machine-type code in the product name (`20HRCTO1WW`) and the model's name in
  the system version (`ThinkPad X1 Carbon 5th`), so a usable Lenovo system
  version is the model.

A placeholder is an empty or whitespace-only string, one without a letter or
digit, a single repeated character (`0000`, `xxxx`), or one of the templates
firmware vendors ship unfilled, compared case-insensitively: `To Be Filled By
O.E.M.`, `System manufacturer`, `System Product Name`, `System Version`, `Default
string`, `Not Applicable`, `Not Specified`, `None`, `N/A`, `Unknown`, `OEM`,
Insyde's `Type1...`/`Type2 - ...` names and the others listed in
`Placeholder()`. Values are cleaned (whitespace collapsed and trimmed, control
characters and bytes that are not UTF-8 dropped) and cut at a whole character
within 91 bytes, what `__system_property_get` returns. A DIY desktop with an
unfilled system structure therefore reports its motherboard (for example
`ASUSTeK COMPUTER INC.` / `PRIME B450M-A`); `Gesher` / `PC` appears only when the
firmware names nothing, and never another vendor's name.

A virtual machine's SMBIOS names its hypervisor (`Microsoft Corporation` /
`Virtual Machine` on Hyper-V and Azure, `VMware, Inc.` / `VMware7,1`, `innotek
GmbH` / `VirtualBox`, `QEMU` / `Standard PC (...)`). That is the machine Gesher
runs on, and is reported as is.

`ro.product.manufacturer` and `ro.product.model` are not in `app/build.prop`;
the values from SMBIOS are applied before and after build.prop is read, so no
build.prop can replace them and they are the same whichever form
`ANDROID_ROOT` takes. Like every `ro.*` property, they are fixed for the boot.

No `ro.product.<partition>.*` variants are set: on Android, init derives
`ro.product.*` from them, and `Build` reads only `ro.product.*`.

## Brand, product and fingerprint

`app/build.prop` gives Gesher's own brand, product, device, board, hardware,
bootloader, flavor and fingerprint. Until 2026-11-25 these named Google's
Pixel 8 (`google/shiba/shiba`, `Google`, `Pixel 8`). `ro.product.first_api_level`
(34) and `ro.sf.lcd_density` (420) are unchanged.

`ANDROID_ROOT` may be a drive-relative path (`/Gesher/system`), a Windows path,
or `/mnt/<drive>/...` ([0045](decisions/0045-posix-paths-tz-procfs.md)); the
property area reads `$ANDROID_ROOT/build.prop` from the same file POSIX `open()`
gives dalvikvm and gesher-app for each of them. Before 2026-11-25 a `/mnt/<drive>`
root was not found there, so `Build.MODEL` was `Gesher` while the rest of
build.prop applied.

## Device name

`Settings.Global.DEVICE_NAME` defaults to Windows' computer name as Settings >
System > About shows it: `GetComputerNameExW(ComputerNamePhysicalDnsHostname)`,
the physical DNS host name with its case kept, not the 15-character upper-case
NetBIOS name (`%COMPUTERNAME%`). `Build.MODEL` is used only if Windows gives no
name, as Android's own default is the model.

Settings rows are stored with their defaults, so Gesher records the name it
derived in `persist.gesher.device_name`. At each start a stored name that is the
recorded one is replaced by the current computer name (renaming the PC renames
the device); so is the fixed `Gesher` every profile stored before this change,
once, while no derived name is recorded. Any other stored name is the user's and
stays, a name the user sets to `Gesher` afterwards stays, and a deleted setting
stays deleted. A user who named the device exactly `Gesher` before this change
gets the computer name.

The Bluetooth adapter's name is the name Windows gives the radio (the computer
name); Windows owns it and `setName` returns false ([bluetooth.md](bluetooth.md)).

## Application compatibility

- Applications that allow-list device models (some games' graphics presets,
  streaming DRM levels, banking apps) no longer see a Pixel 8. They see the real
  PC, as they would see any device they do not list.
- Emulator heuristics look for `generic`, `unknown`, `sdk`, `goldfish`, `ranchu`,
  `vbox86`, `Genymotion` or `Android SDK built for x86`; none of Gesher's values
  contains them. On a virtual machine `Build.MODEL` honestly names it (for
  example `Virtual Machine`), which such heuristics may notice.
- Google Play device registration and Play Integrity: the Pixel 8 fingerprint
  claimed a certified Google build that Gesher is not. With Gesher's own
  fingerprint the device registers as what it is; verdicts that require a
  certified device are not expected to pass (not verified here). A profile
  that microG already registered as a Pixel 8 may appear in Play's device list
  under the new name after its next check-in (not verified here).

## Tests

- `tools/test_device_identity.py` runs `tests/native/device_identity_test.cc`
  on the production header: vendor tables, placeholders, the baseboard and
  Gesher fallbacks, virtual machines, cleaning and truncation, malformed and
  random tables. It also checks `app/build.prop`'s fingerprint against its
  fields and that no Pixel/Google identity remains.
- `tests/windows/tracks/43-exec2.ps1` runs `getprop` in a new property area
  with `ANDROID_ROOT` drive-relative and as `/mnt/<drive>/...`, and compares the
  manufacturer and model with Windows' `Win32_ComputerSystem` (SMBIOS) and the
  fingerprint with its fields.
- `SettingsProviderContractTest` checks the device name default against
  Windows' computer name and `%COMPUTERNAME%`, the legacy and renamed-PC
  updates, and that a user's or deleted name stays.

Not covered by CI: physical PCs with placeholder SMBIOS strings (only the
parser's tables), and Play's web listing itself.
