# Windows USB storage probe

Use this standalone probe to check whether Windows exposes a USB card reader
or storage device through its disk interface and permits a raw read. The probe
uses the installed Windows disk driver and opens media for reading only.

Build on Windows with CMake and the Visual Studio C++ tools, or use the
repository's LLVM-MinGW toolchain.

```powershell
cmake -S probes/usb-storage -B out/usb-storage
cmake --build out/usb-storage --config Release
```

Run `usb_storage_probe.exe --list` to list present disk interface paths whose
Windows storage descriptors report a USB bus. An empty result means that
the probe could not identify a present USB disk interface. Some readers
register a disk interface only when media is inserted.

Use `usb_storage_probe.exe --read "<device-path>" <byte-offset> <byte-count>` to
read up to 1 MiB. Pass a path from the list and align both values to the
disk's logical sector size. The read command reports errors on stderr and
emits the requested bytes on stdout. Use binary-safe output redirection if
capturing the bytes. A raw read requires the corresponding Windows access
rights. A card reader may enumerate even when it denies or cannot serve a
read.

The probe checks the bus type on the same read handle used for the transfer.
It is a host diagnostic for [#407](https://github.com/gesher-runtime/gesher/issues/407).
The separate `gesher_usb_block_physical_test` target in the main CMake project
exercises the raw block broker and Gesher's guest paths. Run the built program
with the selected path from `--list`. It opens a real device read-only and
compares reads of the first and last logical sectors through the host and
guest paths. The ordinary CI fixture instead uses a private test-only peer.

WinUSB and USB storage use different Windows drivers and Android interfaces.
The raw storage path requires an authenticated guest launch that carries the
holder's broker capability. The standard per-user installation has no such
privileged launch authority for CardWake's `su` commands.

References

- [Windows disk interface enumeration](https://learn.microsoft.com/en-us/windows/win32/fileio/disk-enumeration)
- [Windows disk geometry and logical sector size](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_disk_get_drive_geometry_ex)
