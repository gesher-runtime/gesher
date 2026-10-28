// CI-only independent parent observer; never compiled or run locally.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

public sealed class BinderOutputPageObserver : IDisposable {
    [StructLayout(LayoutKind.Explicit, Size = 48)]
    private struct Region {
        [FieldOffset(0)] public ulong BaseAddress;
        [FieldOffset(24)] public ulong RegionSize;
        [FieldOffset(32)] public uint State;
        [FieldOffset(36)] public uint Protect;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address, out Region region, UIntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] bytes, UIntPtr size, out UIntPtr copied);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
    private IntPtr process;
    public BinderOutputPageObserver(uint pid) {
        if (IntPtr.Size != 8 || pid == 0) throw new ArgumentException("Expected actual Win64 actor PID");
        process = OpenProcess(0x0400 | 0x0010, false, pid);
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public string Inspect(ulong address, int size, uint protection, bool sentinel) {
        if (process == IntPtr.Zero || size != Environment.SystemPageSize || address % (uint)size != 0)
            throw new ArgumentException("Expected one aligned actual page");
        Region region;
        if (VirtualQueryEx(process, new IntPtr(checked((long)address)), out region, new UIntPtr(48)).ToUInt64() != 48 ||
            region.State != 0x1000 || region.Protect != protection || region.BaseAddress > address ||
            region.RegionSize < (ulong)size || address - region.BaseAddress > region.RegionSize - (ulong)size)
            throw new InvalidOperationException("Actual page protection/range differs");
        byte[] bytes = new byte[size];
        UIntPtr copied;
        if (!ReadProcessMemory(process, new IntPtr(checked((long)address)), bytes, new UIntPtr((uint)size), out copied) || copied.ToUInt64() != (ulong)size)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (sentinel) foreach (byte value in bytes) if (value != 0x5a) throw new InvalidOperationException("Actual whole-page sentinel changed");
        using (SHA256 hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
    }
    public void Dispose() {
        if (process == IntPtr.Zero) return;
        if (!CloseHandle(process)) throw new Win32Exception(Marshal.GetLastWin32Error());
        process = IntPtr.Zero;
    }
}
