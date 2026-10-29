// CI fixture supervisor: an outer job owns the protocol owner and all actors.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class TrustedSgJob {
    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimits {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr Minimum, Maximum;
        public uint ActiveLimit;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IoCounters { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)]
    struct ExtendedLimits {
        public BasicLimits Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcess, PeakJob;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct Accounting {
        public long TotalUser, TotalKernel, ThisPeriodUser, ThisPeriodKernel;
        public uint PageFaults, TotalProcesses, ActiveProcesses, TerminatedProcesses;
    }
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateJobObjectW(IntPtr security, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetInformationJobObject(IntPtr job, int type, ref ExtendedLimits info, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool QueryInformationJobObject(IntPtr job, int type, out Accounting info, uint size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CloseHandle(IntPtr handle);
    [StructLayout(LayoutKind.Sequential)]
    struct Security { public uint Length; public IntPtr Descriptor; [MarshalAs(UnmanagedType.Bool)] public bool Inherit; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Startup {
        public uint Size; public IntPtr Reserved, Desktop, Title;
        public uint X, Y, Width, Height, CharWidth, CharHeight, Fill, Flags;
        public ushort Show, ReservedBytes; public IntPtr ReservedData, In, Out, Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct StartupEx { public Startup Startup; public IntPtr Attributes; }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInformation { public IntPtr Process, Thread; public uint Pid, Tid; }
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CreatePipe(out IntPtr read, out IntPtr write, ref Security security, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref UIntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute,
        IntPtr value, UIntPtr size, IntPtr previous, IntPtr returned);
    [DllImport("kernel32.dll")]
    static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processSecurity,
        IntPtr threadSecurity, bool inherit, uint flags, IntPtr environment, string directory,
        ref StartupEx startup, out ProcessInformation process);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern uint GetProcessId(IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetProcessTimes(IntPtr process, out long created, out long exited, out long kernel, out long user);

    public sealed class Owner {
        public IntPtr Job, Process;
        public StreamWriter Input;
        public StreamReader Output, Error;
        public SafeFileHandle InputHandle, OutputHandle, ErrorHandle;
        public IntPtr InputRaw, OutputRaw, ErrorRaw;
        public uint Pid;
    }
    // On an unconfirmed startup rollback keep every raw handle for process lifetime.
    static readonly IntPtr[] Quarantine = new IntPtr[128];
    static readonly Owner[] QuarantineOwners = new Owner[32];
    static int quarantineCount;
    static int quarantineOwnerCount;
    public static bool LastStartupRollbackConfirmed;
    static void Retain(IntPtr handle) {
        if (handle == IntPtr.Zero) return;
        lock (Quarantine) {
            if (quarantineCount == Quarantine.Length) Environment.FailFast("SG startup quarantine exhausted");
            Quarantine[quarantineCount++] = handle;
        }
    }
    static void CloseOwned(ref IntPtr handle) {
        IntPtr current = handle;
        handle = IntPtr.Zero;
        if (current != IntPtr.Zero && !CloseHandle(current)) {
            int error = Marshal.GetLastWin32Error();
            Retain(current);
            throw new Win32Exception(error);
        }
    }
    static void Require(bool ok) { if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error()); }

    static IntPtr CreateJob() {
        IntPtr job = CreateJobObjectW(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        ExtendedLimits limits = new ExtendedLimits();
        limits.Basic.Flags = 0x2000;
        if (!SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits)))) {
            int error = Marshal.GetLastWin32Error();
            if (!CloseHandle(job)) Retain(job);
            throw new Win32Exception(error);
        }
        return job;
    }

    // python and script are absolute host-controlled paths without quote characters.
    public static Owner Start(string python, string script, int fixtureFault) {
        LastStartupRollbackConfirmed = false;
        if (python.IndexOf('"') >= 0 || script.IndexOf('"') >= 0 ||
            python.EndsWith("\\") || script.EndsWith("\\"))
            throw new ArgumentException("invalid owner path");
        IntPtr job = IntPtr.Zero, inputRead = IntPtr.Zero, inputWrite = IntPtr.Zero;
        IntPtr outputRead = IntPtr.Zero, outputWrite = IntPtr.Zero, errorRead = IntPtr.Zero, errorWrite = IntPtr.Zero;
        IntPtr attrs = IntPtr.Zero, handles = IntPtr.Zero, jobs = IntPtr.Zero;
        bool initialized = false, success = false;
        ProcessInformation pi = new ProcessInformation();
        Owner partial = null;
        try {
            job = CreateJob();
            Security security = new Security { Length = (uint)Marshal.SizeOf(typeof(Security)), Inherit = true };
            Require(CreatePipe(out inputRead, out inputWrite, ref security, 0));
            Require(CreatePipe(out outputRead, out outputWrite, ref security, 0));
            Require(CreatePipe(out errorRead, out errorWrite, ref security, 0));
            Require(SetHandleInformation(inputWrite, 1, 0));
            Require(SetHandleInformation(outputRead, 1, 0));
            Require(SetHandleInformation(errorRead, 1, 0));
            UIntPtr size = UIntPtr.Zero;
            InitializeProcThreadAttributeList(IntPtr.Zero, 2, 0, ref size);
            if (size == UIntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            attrs = Marshal.AllocHGlobal(checked((int)size.ToUInt64()));
            Require(InitializeProcThreadAttributeList(attrs, 2, 0, ref size));
            initialized = true;
            handles = Marshal.AllocHGlobal(IntPtr.Size * 3);
            Marshal.WriteIntPtr(handles, 0, inputRead);
            Marshal.WriteIntPtr(handles, IntPtr.Size, outputWrite);
            Marshal.WriteIntPtr(handles, IntPtr.Size * 2, errorWrite);
            jobs = Marshal.AllocHGlobal(IntPtr.Size);
            Marshal.WriteIntPtr(jobs, job);
            Require(UpdateProcThreadAttribute(attrs, 0, new IntPtr(0x20002), handles,
                (UIntPtr)(uint)(IntPtr.Size * 3), IntPtr.Zero, IntPtr.Zero));
            Require(UpdateProcThreadAttribute(attrs, 0, new IntPtr(0x2000d), jobs,
                (UIntPtr)(uint)IntPtr.Size, IntPtr.Zero, IntPtr.Zero));
            StartupEx startup = new StartupEx();
            startup.Startup.Size = (uint)Marshal.SizeOf(typeof(StartupEx));
            startup.Startup.Flags = 0x100;
            startup.Startup.In = inputRead; startup.Startup.Out = outputWrite; startup.Startup.Error = errorWrite;
            startup.Attributes = attrs;
            StringBuilder command = new StringBuilder('"' + python + "\" \"" + script + '"');
            Require(CreateProcessW(python, command, IntPtr.Zero, IntPtr.Zero, true,
                0x08080004, IntPtr.Zero, null, ref startup, out pi));
            // JOB_LIST assignment and explicit HANDLE_LIST are atomic at creation.
            bool inJob; long created, exited, kernel, user;
            Require(IsProcessInJob(pi.Process, job, out inJob));
            Require(GetProcessTimes(pi.Process, out created, out exited, out kernel, out user));
            if (!inJob || pi.Pid == 0 || GetProcessId(pi.Process) != pi.Pid || created == 0)
                throw new InvalidOperationException("owner identity or job membership mismatch");
            if (ResumeThread(pi.Thread) != 1) throw new InvalidOperationException("unexpected owner suspend count");
            CloseOwned(ref pi.Thread);
            CloseOwned(ref inputRead);
            CloseOwned(ref outputWrite);
            CloseOwned(ref errorWrite);
            partial = new Owner { Job = job, Process = pi.Process, Pid = pi.Pid };
            partial.InputRaw = inputWrite;
            inputWrite = IntPtr.Zero;
            partial.InputHandle = new SafeFileHandle(partial.InputRaw, false);
            if (fixtureFault == 1) throw new InvalidOperationException("fixture input handle");
            partial.Input = new StreamWriter(new FileStream(partial.InputHandle, FileAccess.Write), new UTF8Encoding(false));
            if (fixtureFault == 2) throw new InvalidOperationException("fixture input stream");
            partial.OutputRaw = outputRead;
            outputRead = IntPtr.Zero;
            partial.OutputHandle = new SafeFileHandle(partial.OutputRaw, false);
            if (fixtureFault == 3) throw new InvalidOperationException("fixture output handle");
            partial.Output = new StreamReader(new FileStream(partial.OutputHandle, FileAccess.Read), Encoding.UTF8);
            if (fixtureFault == 4) throw new InvalidOperationException("fixture output stream");
            partial.ErrorRaw = errorRead;
            errorRead = IntPtr.Zero;
            partial.ErrorHandle = new SafeFileHandle(partial.ErrorRaw, false);
            if (fixtureFault == 5) throw new InvalidOperationException("fixture error handle");
            partial.Error = new StreamReader(new FileStream(partial.ErrorHandle, FileAccess.Read), Encoding.UTF8);
            if (fixtureFault == 6) throw new InvalidOperationException("fixture error stream");
            success = true;
            return partial;
        } finally {
            if (initialized) DeleteProcThreadAttributeList(attrs);
            if (attrs != IntPtr.Zero) Marshal.FreeHGlobal(attrs);
            if (handles != IntPtr.Zero) Marshal.FreeHGlobal(handles);
            if (jobs != IntPtr.Zero) Marshal.FreeHGlobal(jobs);
            foreach (IntPtr handle in new[] { inputRead, inputWrite, outputRead, outputWrite, errorRead, errorWrite, pi.Thread })
                if (handle != IntPtr.Zero && !CloseHandle(handle)) Retain(handle);
            if (!success && job != IntPtr.Zero) {
                TerminateJobObject(job, 137);
                bool empty = false;
                try {
                    long deadline = Environment.TickCount64 + 5000;
                    do {
                        empty = Active(job) == 0;
                        if (!empty) System.Threading.Thread.Sleep(10);
                    } while (!empty && Environment.TickCount64 < deadline);
                } catch { }
                if (empty && (pi.Process == IntPtr.Zero || WaitForSingleObject(pi.Process, 5000) == 0)) {
                    bool streamsClosed = true;
                    if (partial != null) {
                        try { CloseStreams(partial); } catch { streamsClosed = false; }
                    }
                    bool processClosed = pi.Process == IntPtr.Zero || CloseHandle(pi.Process);
                    bool jobClosed = CloseHandle(job);
                    if (!processClosed) Retain(pi.Process);
                    if (!jobClosed) Retain(job);
                    if (!streamsClosed && partial != null) QuarantineOwner(partial);
                    LastStartupRollbackConfirmed = streamsClosed && processClosed && jobClosed;
                } else {
                    Retain(pi.Process); Retain(job);
                    if (partial != null) QuarantineOwner(partial);
                }
            }
        }
    }

    public static bool Wait(Owner owner, uint milliseconds) { return WaitForSingleObject(owner.Process, milliseconds) == 0; }
    public static uint ExitCode(Owner owner) { uint code; Require(GetExitCodeProcess(owner.Process, out code)); return code; }
    public static void CloseProcess(Owner owner) { CloseOwned(ref owner.Process); }
    public static void CloseJob(Owner owner) { CloseOwned(ref owner.Job); }
    public static void CloseStreams(Owner owner) {
        // SafeFileHandles are non-owning: retirement of every managed reader
        // precedes a single explicit, checked close of each raw pipe handle.
        if (owner.Input != null) owner.Input.Dispose();
        if (owner.Output != null) owner.Output.Dispose();
        if (owner.Error != null) owner.Error.Dispose();
        if (owner.InputHandle != null) owner.InputHandle.Dispose();
        if (owner.OutputHandle != null) owner.OutputHandle.Dispose();
        if (owner.ErrorHandle != null) owner.ErrorHandle.Dispose();
        CloseOwned(ref owner.InputRaw);
        CloseOwned(ref owner.OutputRaw);
        CloseOwned(ref owner.ErrorRaw);
    }
    public static void QuarantineOwner(Owner owner) {
        lock (Quarantine) {
            if (quarantineOwnerCount == QuarantineOwners.Length) Environment.FailFast("SG owner quarantine exhausted");
            QuarantineOwners[quarantineOwnerCount++] = owner;
        }
    }
    public static uint Active(IntPtr job) {
        Accounting info;
        Require(QueryInformationJobObject(job, 1, out info, (uint)Marshal.SizeOf(typeof(Accounting)), IntPtr.Zero));
        return info.ActiveProcesses;
    }
    public static void Terminate(IntPtr job) { Require(TerminateJobObject(job, 137)); }
    public static void Close(IntPtr job) { Require(CloseHandle(job)); }
}
