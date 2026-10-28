// CI-only Win32 controller. No redirected pipes, reader tasks, Process disposal or PID killing.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class TsfJobSupervisor {
  const uint WAIT_OBJECT_0=0, WAIT_TIMEOUT=258;
  const long OutputCap=1048576;
  [StructLayout(LayoutKind.Sequential)] struct SA { public int size; public IntPtr descriptor; public int inherit; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct SI {
    public int size; public string reserved, desktop, title; public uint x,y,dx,dy,xc,yc,fill,flags;
    public ushort show, reservedSize; public IntPtr reservedBytes, input, output, error;
  }
  [StructLayout(LayoutKind.Sequential)] struct SIX { public SI si; public IntPtr attributes; }
  [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr process, thread; public uint pid, tid; }
  [StructLayout(LayoutKind.Sequential)] struct Limits {
    public long processTime,jobTime; public uint flags; public UIntPtr min,max;
    public uint active; public UIntPtr affinity; public uint priority,scheduling;
  }
  [StructLayout(LayoutKind.Sequential)] struct IO { public ulong a,b,c,d,e,f; }
  [StructLayout(LayoutKind.Sequential)] struct Extended { public Limits basic; public IO io; public UIntPtr pm,jm,pp,pj; }
  [StructLayout(LayoutKind.Sequential)] struct Accounting {
    public long user,kernel,periodUser,periodKernel; public uint faults,total,active,terminated;
  }
  [StructLayout(LayoutKind.Sequential)] struct ThreadEntry {
    public uint size,usage,id,pid; public int priority,delta; public uint flags;
  }
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr sa,string name);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int cls,ref Extended info,uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int cls,out Accounting info,uint size,IntPtr length);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool IsProcessInJob(IntPtr process,IntPtr job,out bool member);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateJobObject(IntPtr job,uint exit);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr process,uint exit);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle,uint ms);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr process,out uint code);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr p,out long birth,out long exit,out long kernel,out long user);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr p,uint flags,StringBuilder image,ref uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags,uint pid);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool Thread32First(IntPtr snapshot,ref ThreadEntry entry);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool Thread32Next(IntPtr snapshot,ref ThreadEntry entry);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern IntPtr CreateFile(string path,uint access,uint share,ref SA sa,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileSizeEx(IntPtr handle,out long length);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list,int count,uint flags,ref UIntPtr bytes);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,IntPtr attr,IntPtr value,UIntPtr size,IntPtr previous,IntPtr returned);
  [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool CreateProcess(string app,StringBuilder command,IntPtr psa,IntPtr tsa,bool inherit,uint flags,IntPtr env,string cwd,ref SIX si,out PI pi);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DuplicateHandle(IntPtr source,IntPtr handle,IntPtr target,out IntPtr copy,uint access,bool inherit,uint options);

  public sealed class Result {
    public uint Pid, ExitCode, TotalProcesses, TerminatedProcesses;
    public uint LastJobActive=UInt32.MaxValue, RootWaitResult=UInt32.MaxValue, PreResumeActive;
    public long RootHandleValue, JobHandleValue;
    public int SuspendedThreadCount;
    public long Birth, MainMilliseconds, CleanupMilliseconds;
    public string Image="", Output="", Error="", Reason="";
    public bool Assigned, MemberBeforeResume, SuspendedSingleThread, RootSignaled, JobEmpty, OutputRead;
    public bool TerminateRequested, TerminateSucceeded, RootHandleClosed, ThreadHandleClosed;
    public int OwnedHandles, ClosedHandles;
    public List<string> Errors=new List<string>();
    public bool Success { get { return Reason=="exit" && ExitCode==0 && RootSignaled && JobEmpty && OutputRead && Errors.Count==0 && OwnedHandles==ClosedHandles; } }
  }
  static Exception Native(string operation) { int error=Marshal.GetLastWin32Error(); return new Win32Exception(error,operation+" Win32="+error); }
  static void Check(bool ok,string operation) { if(!ok) throw Native(operation); }
  static bool Valid(IntPtr h) { return h!=IntPtr.Zero && h!=new IntPtr(-1); }
  static bool CloseOwned(List<IntPtr> owned,IntPtr h,Result r) {
    int index=owned.IndexOf(h);
    if(index<0 || !Valid(h)) { r.Errors.Add("Invalid owned close"); return false; }
    if(!CloseHandle(h)) { r.Errors.Add(Native("Close owned handle").Message); return false; }
    owned[index]=IntPtr.Zero; r.ClosedHandles++; return true;
  }
  static Accounting Count(IntPtr h) { Accounting a; Check(QueryInformationJobObject(h,1,out a,(uint)Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero),"QueryInformationJobObject"); return a; }
  static string Snapshot(string path) {
    // Called only after every owned process is gone; at most cap bytes allocated/read.
    using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)) {
      if(file.Length>OutputCap) throw new IOException("Output cap exceeded");
      byte[] data=new byte[(int)file.Length]; int used=0;
      while(used<data.Length) { int n=file.Read(data,used,data.Length-used); if(n==0) throw new IOException("Short output read"); used+=n; }
      return Encoding.UTF8.GetString(data);
    }
  }
  // mainMs is fixed at 45000 for the real probe; shorter only in controlled CI tests.
  public static Result Run(string executable,string arguments,int mainMs,string fault) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    if(mainMs<1 || mainMs>45000 || (fault!="" && fault!="terminate-denied" && fault!="query-denied")) throw new ArgumentException();
    Result r=new Result(); var owned=new List<IntPtr>();
    IntPtr job=IntPtr.Zero,query=IntPtr.Zero,denied=IntPtr.Zero,root=IntPtr.Zero,thread=IntPtr.Zero;
    IntPtr output=IntPtr.Zero,error=IntPtr.Zero,attrs=IntPtr.Zero,handles=IntPtr.Zero;
    bool attrsReady=false; string dir=null,stdout=null,stderr=null; bool created=false;
    Stopwatch main=Stopwatch.StartNew(); Stopwatch cleanup=null;
    Func<IntPtr,IntPtr> own=h=> { if(!Valid(h)) throw Native("Create owned handle"); owned.Add(h); r.OwnedHandles++; return h; };
    try {
      dir=Path.Combine(Path.GetTempPath(),"tsf-job-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(dir);
      stdout=Path.Combine(dir,"stdout"); stderr=Path.Combine(dir,"stderr");
      SA sa=new SA {size=Marshal.SizeOf(typeof(SA)),inherit=1};
      output=own(CreateFile(stdout,0x40000000,3,ref sa,1,0x100,IntPtr.Zero));
      error=own(CreateFile(stderr,0x40000000,3,ref sa,1,0x100,IntPtr.Zero));
      IntPtr input=own(CreateFile("NUL",0x80000000,3,ref sa,3,0,IntPtr.Zero));
      job=own(CreateJobObject(IntPtr.Zero,null)); query=job; r.JobHandleValue=job.ToInt64();
      Extended limits=new Extended(); limits.basic.flags=0x2000; // KILL_ON_JOB_CLOSE; neither breakaway flag allowed.
      Check(SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf(typeof(Extended))),"Set job limits");
      if(fault!="") {
        IntPtr copy; Check(DuplicateHandle(GetCurrentProcess(),job,GetCurrentProcess(),out copy,fault=="terminate-denied"?4u:1u,false,0),"Duplicate restricted job handle");
        denied=own(copy); if(fault=="query-denied") query=denied;
      }
      UIntPtr bytes=UIntPtr.Zero; InitializeProcThreadAttributeList(IntPtr.Zero,1,0,ref bytes);
      if(bytes==UIntPtr.Zero) throw Native("Attribute list size");
      attrs=Marshal.AllocHGlobal(checked((int)bytes.ToUInt64()));
      Check(InitializeProcThreadAttributeList(attrs,1,0,ref bytes),"Initialize attributes"); attrsReady=true;
      handles=Marshal.AllocHGlobal(3*IntPtr.Size);
      Marshal.WriteIntPtr(handles,0,input); Marshal.WriteIntPtr(handles,IntPtr.Size,output); Marshal.WriteIntPtr(handles,2*IntPtr.Size,error);
      Check(UpdateProcThreadAttribute(attrs,0,new IntPtr(0x20002),handles,new UIntPtr((uint)(3*IntPtr.Size)),IntPtr.Zero,IntPtr.Zero),"Whitelist inherited handles");
      SIX start=new SIX(); start.si.size=Marshal.SizeOf(typeof(SIX)); start.si.flags=0x100; start.si.input=input; start.si.output=output; start.si.error=error; start.attributes=attrs;
      PI pi; executable=Path.GetFullPath(executable);
      Check(CreateProcess(executable,new StringBuilder("\""+executable+"\" "+arguments),IntPtr.Zero,IntPtr.Zero,true,0x08080004,IntPtr.Zero,Path.GetDirectoryName(executable),ref start,out pi),"Create suspended child");
      root=own(pi.process); thread=own(pi.thread); created=true; r.Pid=pi.pid; r.RootHandleValue=root.ToInt64();
      long exit,kernel,user; Check(GetProcessTimes(root,out r.Birth,out exit,out kernel,out user),"Capture root birth");
      var image=new StringBuilder(32768); uint length=(uint)image.Capacity; Check(QueryFullProcessImageName(root,0,image,ref length),"Capture suspended root image"); r.Image=image.ToString();
      if(!String.Equals(r.Image,executable,StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Root image mismatch");
      IntPtr snapshot=own(CreateToolhelp32Snapshot(4,0)); ThreadEntry entry=new ThreadEntry(); entry.size=(uint)Marshal.SizeOf(typeof(ThreadEntry));
      Check(Thread32First(snapshot,ref entry),"Thread32First"); int threads=0;
      do { if(entry.pid==r.Pid) threads++; entry.size=(uint)Marshal.SizeOf(typeof(ThreadEntry)); } while(Thread32Next(snapshot,ref entry));
      r.SuspendedThreadCount=threads;
      if(Marshal.GetLastWin32Error()!=18 || threads!=1) throw new InvalidOperationException("Suspended root thread census failed"); r.SuspendedSingleThread=true;
      if(!CloseOwned(owned,snapshot,r)) throw new IOException("Thread census handle closure failed");
      Check(AssignProcessToJobObject(job,root),"Assign suspended root"); r.Assigned=true;
      bool member; Check(IsProcessInJob(root,job,out member),"Validate job membership"); r.MemberBeforeResume=member;
      Accounting before=Count(job); r.PreResumeActive=before.active; if(!member || before.active!=1 || before.total!=1) throw new InvalidOperationException("Pre-resume job census failed");
      if(ResumeThread(thread)!=1) throw Native("Resume exactly one suspension");
      while(true) {
        long remaining=mainMs-main.ElapsedMilliseconds;
        if(remaining<=0) { r.Reason="timeout"; break; }
        uint wait=WaitForSingleObject(root,(uint)Math.Min(25,remaining));
        if(wait==WAIT_OBJECT_0) { r.Reason="exit"; break; }
        if(wait!=WAIT_TIMEOUT) throw Native("Root wait");
        long a,b; Check(GetFileSizeEx(output,out a),"Stdout size"); Check(GetFileSizeEx(error,out b),"Stderr size");
        if(a>OutputCap || b>OutputCap) { r.Reason="output-limit"; break; }
      }
    } catch(Exception ex) { r.Errors.Add(ex.Message); r.Reason="controller-error"; }
    finally {
      r.MainMilliseconds=main.ElapsedMilliseconds; cleanup=Stopwatch.StartNew();
      // One absolute cleanup budget. No stage receives a reset or an unbounded wait.
      if(created) {
        try {
          if(r.Assigned) {
            Accounting initial=Count(job);
            if(initial.active!=0) {
              if(fault=="terminate-denied") {
                if(TerminateJobObject(denied,124)) r.Errors.Add("Restricted terminate unexpectedly succeeded");
                else r.Errors.Add("Terminate denied Win32="+Marshal.GetLastWin32Error());
              }
              r.TerminateRequested=true;
              Check(TerminateJobObject(job,124),"Terminate owned job"); r.TerminateSucceeded=true;
            }
          } else Check(TerminateProcess(root,124),"Terminate suspended unassigned root");
        } catch(Exception ex) { r.Errors.Add(ex.Message); }
        while(cleanup.ElapsedMilliseconds<5000) {
          try {
            if(!r.RootSignaled) {
              uint wait=WaitForSingleObject(root,0); r.RootWaitResult=wait;
              if(wait!=WAIT_OBJECT_0 && wait!=WAIT_TIMEOUT) throw Native("Cleanup root wait"); r.RootSignaled=wait==WAIT_OBJECT_0;
              if(r.RootSignaled) {
                uint code; Check(GetExitCodeProcess(root,out code),"Root exit code"); r.ExitCode=code;
                // Release our process references before requiring accounting active==0.
                r.ThreadHandleClosed=CloseOwned(owned,thread,r);
                r.RootHandleClosed=CloseOwned(owned,root,r);
                if(!r.ThreadHandleClosed || !r.RootHandleClosed) throw new IOException("Root handle closure failed");
              }
            }
            if(r.Assigned && r.RootSignaled) { Accounting a=Count(query); r.TotalProcesses=a.total; r.TerminatedProcesses=a.terminated; r.LastJobActive=a.active; r.JobEmpty=a.active==0; }
            else r.JobEmpty=false;
            if(r.RootSignaled && (r.JobEmpty || !r.Assigned)) break;
          } catch(Exception ex) { r.Errors.Add(ex.Message); break; }
          int remaining=5000-(int)cleanup.ElapsedMilliseconds; if(remaining>0) Thread.Sleep(Math.Min(10,remaining));
        }
        if(!r.RootSignaled || !r.JobEmpty) r.Errors.Add("Cleanup unproved within shared 5000ms budget");
      }
      for(int i=owned.Count-1;i>=0;i--) { if(Valid(owned[i])) CloseOwned(owned,owned[i],r); }
      if(attrsReady) DeleteProcThreadAttributeList(attrs);
      if(handles!=IntPtr.Zero) Marshal.FreeHGlobal(handles);
      if(attrs!=IntPtr.Zero) Marshal.FreeHGlobal(attrs);
      if(cleanup.ElapsedMilliseconds>=5000) r.Errors.Add("Cleanup budget exhausted including handle closure");
      r.CleanupMilliseconds=cleanup.ElapsedMilliseconds;
    }
    // Files are local owned regular files. No reader exists during the process lifetime.
    if(r.RootSignaled && r.JobEmpty && r.OwnedHandles==r.ClosedHandles && cleanup.ElapsedMilliseconds<5000) {
      try { r.Output=Snapshot(stdout); r.Error=Snapshot(stderr); r.OutputRead=true; } catch(Exception ex) { r.Errors.Add(ex.Message); }
    }
    try { if(stdout!=null && File.Exists(stdout)) File.Delete(stdout); if(stderr!=null && File.Exists(stderr)) File.Delete(stderr); if(dir!=null) Directory.Delete(dir); }
    catch(Exception ex) { r.Errors.Add("Owned temporary file cleanup: "+ex.Message); }
    r.CleanupMilliseconds=cleanup.ElapsedMilliseconds;
    if(r.CleanupMilliseconds>=5000) r.Errors.Add("Cleanup budget exhausted including output/file cleanup");
    return r;
  }
}
