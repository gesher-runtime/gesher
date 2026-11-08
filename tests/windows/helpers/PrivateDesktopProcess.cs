using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

// Owned synthetic fixtures. Private desktops are never switched; callers on
// the current desktop keep their own windows hidden.
public static class PrivateDesktopProcess {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Startup {
    public uint cb; public string reserved, desktop, title;
    public uint x,y,width,height,charsX,charsY,fill,flags; public ushort show,reservedBytes;
    public IntPtr reservedData, input, output, error;
  }
  [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr process,thread; public uint pid,tid; }
  [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
    public long processTime,jobTime; public uint flags; public UIntPtr minWork,maxWork;
    public uint activeProcesses; public UIntPtr affinity; public uint priority,scheduling;
  }
  [StructLayout(LayoutKind.Sequential)] struct IoCounters { public ulong a,b,c,d,e,f; }
  [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
    public BasicLimits basic; public IoCounters io; public UIntPtr processMemory,jobMemory,peakProcessMemory,peakJobMemory;
  }
  [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateDesktopW(string n,IntPtr d,IntPtr m,uint f,uint a,IntPtr s);
  [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcessW(string app,StringBuilder cmd,IntPtr p,IntPtr t,bool inherit,uint flags,IntPtr env,string cwd,ref Startup start,out ProcessInfo info);
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateJobObjectW(IntPtr a,IntPtr n);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr h,int c,ref ExtendedLimits i,uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr j,IntPtr p);
  [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr t);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h,uint ms);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr p,out uint code);
  [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr p,uint code);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static int Run(string command,string cwd,int timeoutMs) {
    return RunOwned(command,cwd,timeoutMs,true);
  }
  public static int RunOnCurrentDesktop(string command,string cwd,int timeoutMs) {
    return RunOwned(command,cwd,timeoutMs,false);
  }
  static int RunOwned(string command,string cwd,int timeoutMs,bool privateDesktop) {
    IntPtr desktop=IntPtr.Zero,job=IntPtr.Zero; ProcessInfo info=new ProcessInfo();
    try {
      string name=null;
      if(privateDesktop) {
        name="GesherFixture-"+Guid.NewGuid().ToString("N");
        desktop=CreateDesktopW(name,IntPtr.Zero,IntPtr.Zero,0,0x01ff,IntPtr.Zero);
        if(desktop==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
      }
      job=CreateJobObjectW(IntPtr.Zero,IntPtr.Zero);
      if(job==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
      ExtendedLimits limits=new ExtendedLimits(); limits.basic.flags=0x2000; // Kill owned descendants on close.
      if(!SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf(typeof(ExtendedLimits)))) throw new Win32Exception(Marshal.GetLastWin32Error());
      Startup start=new Startup(); start.cb=(uint)Marshal.SizeOf(typeof(Startup)); start.desktop=name;
      if(!CreateProcessW(null,new StringBuilder(command),IntPtr.Zero,IntPtr.Zero,false,0x08000004,IntPtr.Zero,cwd,ref start,out info)) throw new Win32Exception(Marshal.GetLastWin32Error());
      if(!AssignProcessToJobObject(job,info.process)) { TerminateProcess(info.process,1); throw new Win32Exception(Marshal.GetLastWin32Error()); }
      if(ResumeThread(info.thread)==0xffffffff) throw new Win32Exception(Marshal.GetLastWin32Error());
      if(WaitForSingleObject(info.process,(uint)timeoutMs)!=0) throw new TimeoutException("Synthetic private desktop process timed out");
      uint code; if(!GetExitCodeProcess(info.process,out code)) throw new Win32Exception(Marshal.GetLastWin32Error());
      return unchecked((int)code);
    } finally {
      if(job!=IntPtr.Zero) CloseHandle(job);
      if(info.thread!=IntPtr.Zero) CloseHandle(info.thread);
      if(info.process!=IntPtr.Zero) CloseHandle(info.process);
      if(desktop!=IntPtr.Zero) CloseDesktop(desktop);
    }
  }
}
