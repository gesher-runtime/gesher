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
  // Failed positive-control retirement remains owned for this CI process's lifetime.
  static readonly List<IntPtr> QuarantinedTestProcesses=new List<IntPtr>();
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
  [StructLayout(LayoutKind.Sequential)] struct ProcessBasicInfo {
    public IntPtr reserved1, peb, reserved2a, reserved2b, processId, parentId;
  }
  [StructLayout(LayoutKind.Sequential)] struct ThreadEntry {
    public uint size,usage,id,pid; public int priority,delta; public uint flags;
  }
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct ProcessEntry {
    public uint size,usage,pid; public UIntPtr heap; public uint module,threads,parent;
    public int priority; public uint flags;
    [MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)] public string exe;
  }
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr sa,string name);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int cls,ref Extended info,uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int cls,out Accounting info,uint size,IntPtr length);
  [DllImport("kernel32.dll",SetLastError=true,EntryPoint="QueryInformationJobObject")] static extern bool QueryExtended(IntPtr job,int cls,out Extended info,uint size,IntPtr length);
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
  [DllImport("kernel32.dll",SetLastError=true,EntryPoint="QueryInformationJobObject")] static extern bool QueryPidList(IntPtr job,int cls,IntPtr list,uint size,IntPtr length);
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
  [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr process,int infoClass,out ProcessBasicInfo info,uint length,out uint returned);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool Process32First(IntPtr snapshot,ref ProcessEntry entry);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool Process32Next(IntPtr snapshot,ref ProcessEntry entry);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern IntPtr CreateFile(string path,uint access,uint share,ref SA sa,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileSizeEx(IntPtr handle,out long length);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list,int count,uint flags,ref UIntPtr bytes);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,IntPtr attr,IntPtr value,UIntPtr size,IntPtr previous,IntPtr returned);
  [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool CreateProcess(string app,StringBuilder command,IntPtr psa,IntPtr tsa,bool inherit,uint flags,IntPtr env,string cwd,ref SIX si,out PI pi);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode,EntryPoint="CreateProcessW")] static extern bool CreateProcessSimple(string app,StringBuilder command,IntPtr psa,IntPtr tsa,bool inherit,uint flags,IntPtr env,string cwd,ref SI si,out PI pi);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DuplicateHandle(IntPtr source,IntPtr handle,IntPtr target,out IntPtr copy,uint access,bool inherit,uint options);

  public sealed class Result {
    public uint Pid, ExitCode, TotalProcesses, TerminatedProcesses;
    public uint LastJobActive=UInt32.MaxValue, RootWaitResult=UInt32.MaxValue, PreResumeActive;
    public uint ActiveProcessLimit;
    public long RootHandleValue, JobHandleValue;
    public int SuspendedThreadCount, DescendantThreadCount, SnapshotWin32Error;
    public long Birth, MainMilliseconds, CleanupMilliseconds;
    public string Image="", Output="", Error="", Reason="";
    public bool Assigned, MemberBeforeResume, SuspendedSingleThread, RootSignaled, JobEmpty, OutputRead;
    public bool TerminateRequested, TerminateSucceeded, RootHandleClosed, ThreadHandleClosed;
    public int OwnedHandles, ClosedHandles;
    public List<string> Errors=new List<string>();
    public List<Identity> Processes=new List<Identity>();
    public uint DescendantPid;
    public long StdoutBytes, StderrBytes, CapturedBytes;
    public bool ControlStarted, DescendantSuspended, RetainedWriterReady, WritersRetired, ExclusiveSnapshot, LimitChildRan;
    public bool Success { get { return Reason=="exit" && ExitCode==0 && RootSignaled && JobEmpty && OutputRead && Errors.Count==0 && OwnedHandles==ClosedHandles; } }
  }
  public sealed class Identity {
    public uint Pid, Parent, WaitResult=UInt32.MaxValue;
    public long Birth, HandleValue; public string Image="", Role="";
    public bool Member, Signaled, Closed;
    internal IntPtr Handle;
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
  static string Snapshot(string path,Result r) {
    // Called only after every owned process is gone; at most cap bytes allocated/read.
    using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.None)) {
      if(file.Length>OutputCap) throw new IOException("Output cap exceeded");
      byte[] data=new byte[(int)file.Length]; int used=0;
      while(used<data.Length) { int n=file.Read(data,used,data.Length-used); if(n==0) throw new IOException("Short output read"); used+=n; }
      r.CapturedBytes+=used; return Encoding.UTF8.GetString(data);
    }
  }
  static List<uint> Pids(IntPtr job) {
    // JOBOBJECT_BASIC_PROCESS_ID_LIST: two DWORDs followed by ULONG_PTR entries.
    // Bounded allocation; ERROR_MORE_DATA or an inconsistent census fails closed.
    const int capacity=64; int size=8+capacity*IntPtr.Size;
    IntPtr list=Marshal.AllocHGlobal(size);
    try {
      Check(QueryPidList(job,3,list,(uint)size,IntPtr.Zero),"Query job PID list");
      int assigned=Marshal.ReadInt32(list),count=Marshal.ReadInt32(list,4);
      if(count<0 || count>capacity || assigned!=count) throw new IOException("Incomplete job PID census");
      var result=new List<uint>();
      for(int i=0;i<count;i++) {
        uint pid=checked((uint)Marshal.ReadIntPtr(list,8+i*IntPtr.Size).ToInt64());
        if(result.Contains(pid)) throw new IOException("Duplicate job PID"); result.Add(pid);
      }
      return result;
    } finally { Marshal.FreeHGlobal(list); }
  }
  // Query the kernel object bound by the retained handle, not a later PID
  // snapshot: an exiting child can disappear from Toolhelp after capture.
  static uint Parent(IntPtr process,uint expectedPid) {
    ProcessBasicInfo info; uint returned;
    int status=NtQueryInformationProcess(process,0,out info,(uint)Marshal.SizeOf(typeof(ProcessBasicInfo)),out returned);
    if(status<0) throw new IOException("Retained process parent query NTSTATUS=0x"+unchecked((uint)status).ToString("X8"));
    if(returned!=Marshal.SizeOf(typeof(ProcessBasicInfo)) || info.processId.ToInt64()!=expectedPid)
      throw new IOException("Retained process parent identity mismatch");
    long parent=info.parentId.ToInt64();
    if(parent<=0 || parent>UInt32.MaxValue) throw new IOException("Invalid retained process parent");
    return (uint)parent;
  }
  // Called only by CI controlled fixture with an actual retained, already
  // signaled child handle; production classification still checks image, job,
  // parent identity and creation-time ordering.
  public static uint TestRetainedParent(IntPtr process,uint expectedPid) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    if(WaitForSingleObject(process,0)!=WAIT_OBJECT_0) throw new IOException("Test child is not signaled");
    return Parent(process,expectedPid);
  }
  public static bool TestToolhelpContains(uint pid) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    IntPtr snapshot=CreateToolhelp32Snapshot(2,0);
    if(!Valid(snapshot)) throw Native("Process snapshot");
    try {
      ProcessEntry entry=new ProcessEntry(); entry.size=(uint)Marshal.SizeOf(typeof(ProcessEntry));
      Check(Process32First(snapshot,ref entry),"Process32First");
      do { if(entry.pid==pid) return true; entry.size=(uint)Marshal.SizeOf(typeof(ProcessEntry)); } while(Process32Next(snapshot,ref entry));
      if(Marshal.GetLastWin32Error()!=18) throw Native("Process32Next");
      return false;
    } finally { Check(CloseHandle(snapshot),"Close process snapshot"); }
  }
  public static bool TestDeniedParent(IntPtr process,uint expectedPid) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    IntPtr restricted;
    Check(DuplicateHandle(GetCurrentProcess(),process,GetCurrentProcess(),out restricted,0x100000,false,0),"Duplicate parent-query denial handle");
    try {
      try { Parent(restricted,expectedPid); return false; }
      catch(IOException e) { return e.Message.Contains("NTSTATUS=0xC0000022"); }
    } finally { Check(CloseHandle(restricted),"Close parent-query denial handle"); }
  }
  public static void TestLimitPositive(string executable,string protocol) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    SI si=new SI(); si.size=Marshal.SizeOf(typeof(SI)); PI child;
    Check(CreateProcessSimple(executable,new StringBuilder("\""+executable+"\" limit-child \""+protocol+"\""),IntPtr.Zero,IntPtr.Zero,false,0x8,IntPtr.Zero,null,ref si,out child),"Positive child creation");
    bool retired=false;
    try {
      retired=WaitForSingleObject(child.process,5000)==WAIT_OBJECT_0;
      if(!retired) {
        bool terminated=TerminateProcess(child.process,124);
        retired=WaitForSingleObject(child.process,5000)==WAIT_OBJECT_0;
        throw new IOException("Positive child wait failed; terminate="+terminated+" retired="+retired);
      }
      uint code; Check(GetExitCodeProcess(child.process,out code),"Positive child exit");
      if(code!=96 || !File.Exists(Path.Combine(protocol,"limit-child-ran"))) throw new IOException("Positive child did not execute");
    } finally {
      bool threadClosed=CloseHandle(child.thread);
      bool processClosed=false;
      if(retired) processClosed=CloseHandle(child.process);
      if(!threadClosed || !processClosed) lock(QuarantinedTestProcesses) {
        if(!threadClosed) QuarantinedTestProcesses.Add(child.thread);
        if(!processClosed) QuarantinedTestProcesses.Add(child.process);
      }
      if(!threadClosed || !processClosed) throw new IOException("Positive child closure unproved: retired="+retired+" thread="+threadClosed+" process="+processClosed);
    }
  }
  static void Census(IntPtr job,Result r,List<IntPtr> owned,Func<IntPtr,IntPtr> own,string executable,string raceProtocol) {
    var ids=Pids(job);
    // The kernel list has no ordering contract. Capture the expected suspended
    // descendant before classifying any console host whose parent is that PID.
    if(r.DescendantPid!=0 && ids.Remove(r.DescendantPid)) ids.Insert(0,r.DescendantPid);
    foreach(uint pid in ids) {
      if(r.Processes.Exists(p=>p.Pid==pid)) continue;
      Identity p=new Identity {Pid=pid}; r.Processes.Add(p);
      p.Handle=own(OpenProcess(0x100000|0x1000,false,pid)); p.HandleValue=p.Handle.ToInt64();
      // Only inspect a live retained handle. Never query an exited image or kill by PID.
      if(WaitForSingleObject(p.Handle,0)!=WAIT_TIMEOUT) throw new IOException("Job identity exited before capture: "+pid);
      long exit,kernel,user; Check(GetProcessTimes(p.Handle,out p.Birth,out exit,out kernel,out user),"Job identity birth");
      var image=new StringBuilder(32768); uint length=(uint)image.Capacity;
      Check(QueryFullProcessImageName(p.Handle,0,image,ref length),"Live job identity image"); p.Image=image.ToString();
      bool member; Check(IsProcessInJob(p.Handle,job,out member),"Job identity membership"); p.Member=member;
      if(raceProtocol!=null && pid==r.DescendantPid) {
        File.WriteAllText(Path.Combine(raceProtocol,"race-go"),"go");
        if(WaitForSingleObject(p.Handle,5000)!=WAIT_OBJECT_0 || TestToolhelpContains(pid)) throw new IOException("Census exit race barrier failed");
        Console.WriteLine("TSF_CENSUS_RACE pid="+pid+" live_capture=yes exited=yes toolhelp=absent");
      }
      p.Parent=Parent(p.Handle,pid);
      Identity parent=r.Processes.Find(q=>q.Pid==p.Parent && (q.Role=="root" || q.Role=="descendant"));
      if(pid==r.DescendantPid && parent!=null && parent.Role=="root" && p.Birth>=parent.Birth && String.Equals(p.Image,executable,StringComparison.OrdinalIgnoreCase)) p.Role="descendant";
      else if(parent!=null && p.Birth>=parent.Birth && !r.Processes.Exists(q=>q.Role=="console-host" && q.Parent==p.Parent) && String.Equals(p.Image,Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"conhost.exe"),StringComparison.OrdinalIgnoreCase)) p.Role="console-host";
      else throw new IOException("Unexpected job identity: "+pid+" parent="+p.Parent+" image="+p.Image);
      if(!p.Member || p.Birth<=0) throw new IOException("Job identity ownership failed");
    }
  }
  // mainMs is fixed at 45000 for the real probe; shorter only in controlled CI tests.
  public static Result Run(string executable,string arguments,int mainMs,string fault) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    if(mainMs<1 || mainMs>45000 || (fault!="" && fault!="terminate-denied" && fault!="query-denied" && fault!="diag-detached-unlimited" && fault!="diag-console-limited" && fault!="diag-console-unlimited")) throw new ArgumentException();
    Result r=new Result(); var owned=new List<IntPtr>();
    IntPtr job=IntPtr.Zero,query=IntPtr.Zero,denied=IntPtr.Zero,root=IntPtr.Zero,thread=IntPtr.Zero;
    IntPtr output=IntPtr.Zero,error=IntPtr.Zero,attrs=IntPtr.Zero,handles=IntPtr.Zero;
    bool attrsReady=false; string dir=null,stdout=null,stderr=null,protocol=null; bool created=false;
    bool race=arguments=="race";
    bool limited=arguments=="limit";
    bool controlled=arguments=="normal" || arguments=="exit23" || arguments=="hang" || arguments=="descendant" || arguments=="unknown" || arguments=="race" || arguments=="orphan" || arguments=="output" || limited;
    limited=limited || !controlled;
    if(fault=="diag-detached-unlimited" || fault=="diag-console-unlimited") limited=false;
    Stopwatch main=Stopwatch.StartNew(); Stopwatch cleanup=null;
    Func<IntPtr,IntPtr> own=h=> { if(!Valid(h)) throw Native("Create owned handle"); owned.Add(h); r.OwnedHandles++; return h; };
    try {
      dir=Path.Combine(Path.GetTempPath(),"tsf-job-"+Guid.NewGuid().ToString("N")); Directory.CreateDirectory(dir);
      stdout=Path.Combine(dir,"stdout"); stderr=Path.Combine(dir,"stderr");
      if(controlled) { protocol=Path.Combine(dir,"protocol"); Directory.CreateDirectory(protocol); arguments+=" \""+protocol+"\""; }
      SA sa=new SA {size=Marshal.SizeOf(typeof(SA)),inherit=1};
      output=own(CreateFile(stdout,0x40000000,3,ref sa,1,0x100,IntPtr.Zero));
      error=own(CreateFile(stderr,0x40000000,3,ref sa,1,0x100,IntPtr.Zero));
      IntPtr input=own(CreateFile("NUL",0x80000000,3,ref sa,3,0,IntPtr.Zero));
      job=own(CreateJobObject(IntPtr.Zero,null)); query=job; r.JobHandleValue=job.ToInt64();
      Extended limits=new Extended(); limits.basic.flags=0x2000; // KILL_ON_JOB_CLOSE; neither breakaway flag allowed.
      if(limited) { limits.basic.flags|=0x8; limits.basic.active=1; }
      Check(SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf(typeof(Extended))),"Set job limits");
      Extended actual; Check(QueryExtended(job,9,out actual,(uint)Marshal.SizeOf(typeof(Extended)),IntPtr.Zero),"Read job limits");
      if(limited && ((actual.basic.flags&0x8)==0 || actual.basic.active!=1)) throw new IOException("Active process limit not enforced");
      r.ActiveProcessLimit=actual.basic.active;
      if(fault=="terminate-denied" || fault=="query-denied") {
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
      bool detached=limited || fault=="diag-detached-unlimited";
      if(fault=="diag-console-limited") detached=false;
      uint creation=detached?0x0008000cu:0x08080004u; // Detached when constrained; controlled descendants retain their original path.
      Check(CreateProcess(executable,new StringBuilder("\""+executable+"\" "+arguments),IntPtr.Zero,IntPtr.Zero,true,creation,IntPtr.Zero,Path.GetDirectoryName(executable),ref start,out pi),"Create suspended child");
      root=own(pi.process); thread=own(pi.thread); created=true; r.Pid=pi.pid; r.RootHandleValue=root.ToInt64();
      long exit,kernel,user; Check(GetProcessTimes(root,out r.Birth,out exit,out kernel,out user),"Capture root birth");
      var image=new StringBuilder(32768); uint length=(uint)image.Capacity; Check(QueryFullProcessImageName(root,0,image,ref length),"Capture suspended root image"); r.Image=image.ToString();
      if(!String.Equals(r.Image,executable,StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Root image mismatch");
      Identity rootIdentity=new Identity {Pid=r.Pid,Birth=r.Birth,Image=r.Image,Role="root",Handle=root,HandleValue=root.ToInt64()};
      r.Processes.Add(rootIdentity);
      IntPtr snapshot=own(CreateToolhelp32Snapshot(4,0)); ThreadEntry entry=new ThreadEntry(); entry.size=(uint)Marshal.SizeOf(typeof(ThreadEntry));
      Check(Thread32First(snapshot,ref entry),"Thread32First"); int threads=0;
      do { if(entry.pid==r.Pid) threads++; entry.size=(uint)Marshal.SizeOf(typeof(ThreadEntry)); } while(Thread32Next(snapshot,ref entry));
      r.SuspendedThreadCount=threads;
      if(Marshal.GetLastWin32Error()!=18 || threads!=1) throw new InvalidOperationException("Suspended root thread census failed"); r.SuspendedSingleThread=true;
      if(!CloseOwned(owned,snapshot,r)) throw new IOException("Thread census handle closure failed");
      Check(AssignProcessToJobObject(job,root),"Assign suspended root"); r.Assigned=true;
      bool member; Check(IsProcessInJob(root,job,out member),"Validate job membership"); r.MemberBeforeResume=member;
      rootIdentity.Member=member;
      Accounting before=Count(job); r.PreResumeActive=before.active; if(!member || before.active!=1 || before.total!=1) throw new InvalidOperationException("Pre-resume job census failed");
      if(ResumeThread(thread)!=1) throw Native("Resume exactly one suspension");
      while(true) {
        long remaining=mainMs-main.ElapsedMilliseconds;
        if(remaining<=0) { r.Reason="timeout"; break; }
        if(controlled) {
          if(File.Exists(Path.Combine(protocol,"started")) && !r.ControlStarted) {
            Census(job,r,owned,own,executable,null); r.ControlStarted=true;
            File.WriteAllText(Path.Combine(protocol,"start-go"),"go");
          }
          if(File.Exists(Path.Combine(protocol,"child")) && !r.DescendantSuspended) {
            r.DescendantPid=UInt32.Parse(File.ReadAllText(Path.Combine(protocol,"child")));
            if(r.DescendantPid==0 || r.DescendantPid==r.Pid) throw new IOException("Invalid descendant PID");
            Census(job,r,owned,own,executable,null);
            Identity child=r.Processes.Find(p=>p.Pid==r.DescendantPid && p.Role=="descendant");
            if(child==null) throw new IOException("Suspended descendant not in job");
            IntPtr childThreads=own(CreateToolhelp32Snapshot(4,0));
            ThreadEntry e=new ThreadEntry(); e.size=(uint)Marshal.SizeOf(typeof(ThreadEntry));
            Check(Thread32First(childThreads,ref e),"Descendant Thread32First");
            do { if(e.pid==r.DescendantPid) r.DescendantThreadCount++; e.size=(uint)Marshal.SizeOf(typeof(ThreadEntry)); } while(Thread32Next(childThreads,ref e));
            if(Marshal.GetLastWin32Error()!=18 || r.DescendantThreadCount!=1) throw new IOException("Suspended descendant thread census failed");
            if(!CloseOwned(owned,childThreads,r)) throw new IOException("Descendant thread census close failed");
            // Worker publishes only while its CREATE_SUSPENDED primary thread is held.
            r.DescendantSuspended=true; File.WriteAllText(Path.Combine(protocol,"child-go"),"go");
          }
          if(File.Exists(Path.Combine(protocol,"unknown"))) {
            Census(job,r,owned,own,executable,null);
            throw new IOException("Unknown child accepted by census");
          }
          if(File.Exists(Path.Combine(protocol,"orphan-ready"))) {
            uint missingParent=UInt32.Parse(File.ReadAllText(Path.Combine(protocol,"orphan-ready")));
            if(TestToolhelpContains(missingParent)) throw new IOException("Orphan parent still in Toolhelp");
            Census(job,r,owned,own,executable,null);
            throw new IOException("Orphan accepted by census");
          }
          if(File.Exists(Path.Combine(protocol,"writer-ready")) && !r.RetainedWriterReady) {
            Identity child=r.Processes.Find(p=>p.Pid==r.DescendantPid && p.Role=="descendant");
            if(child==null || WaitForSingleObject(child.Handle,0)!=WAIT_TIMEOUT) throw new IOException("Retainer startup not live");
            Census(job,r,owned,own,executable,null); r.RetainedWriterReady=true;
            File.WriteAllText(Path.Combine(protocol,"writer-go"),"go");
          }
          if(race && File.Exists(Path.Combine(protocol,"race-child")) && r.DescendantPid==0) {
            r.DescendantPid=UInt32.Parse(File.ReadAllText(Path.Combine(protocol,"race-child")));
            Census(job,r,owned,own,executable,protocol);
            File.WriteAllText(Path.Combine(protocol,"race-done"),"go");
          }
        } else Census(job,r,owned,own,executable,null);
        remaining=mainMs-main.ElapsedMilliseconds;
        if(remaining<=0) { r.Reason="timeout"; break; }
        uint wait=WaitForSingleObject(root,(uint)Math.Min(25,remaining));
        if(wait==WAIT_OBJECT_0) { r.Reason="exit"; break; }
        if(wait!=WAIT_TIMEOUT) throw Native("Root wait");
        Check(GetFileSizeEx(output,out r.StdoutBytes),"Stdout size"); Check(GetFileSizeEx(error,out r.StderrBytes),"Stderr size");
        if(r.StdoutBytes>OutputCap || r.StderrBytes>OutputCap) { r.Reason="output-limit"; break; }
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
                Identity p=r.Processes.Find(q=>q.Role=="root"); if(p!=null) { p.WaitResult=wait; p.Signaled=true; p.Closed=r.RootHandleClosed; }
                if(!r.ThreadHandleClosed || !r.RootHandleClosed) throw new IOException("Root handle closure failed");
              }
            }
            foreach(Identity p in r.Processes) {
              if(p.Role=="root" || p.Closed || !Valid(p.Handle)) continue;
              p.WaitResult=WaitForSingleObject(p.Handle,0);
              if(p.WaitResult!=WAIT_OBJECT_0 && p.WaitResult!=WAIT_TIMEOUT) throw Native("Owned job identity wait");
              if(p.WaitResult==WAIT_OBJECT_0) { p.Signaled=true; p.Closed=CloseOwned(owned,p.Handle,r); }
            }
            if(r.Assigned && r.RootSignaled) { Accounting a=Count(query); r.TotalProcesses=a.total; r.TerminatedProcesses=a.terminated; r.LastJobActive=a.active; r.JobEmpty=a.active==0; }
            else r.JobEmpty=false;
            r.WritersRetired=r.Processes.TrueForAll(p=>p.Signaled && p.Closed);
            if(r.RootSignaled && r.WritersRetired && (r.JobEmpty || !r.Assigned)) break;
          } catch(Exception ex) { r.Errors.Add(ex.Message); break; }
          int remaining=5000-(int)cleanup.ElapsedMilliseconds; if(remaining>0) Thread.Sleep(Math.Min(10,remaining));
        }
        if(!r.RootSignaled || !r.JobEmpty || !r.WritersRetired) r.Errors.Add("Cleanup unproved within shared 5000ms budget");
        if(r.JobEmpty && r.TotalProcesses!=r.Processes.Count) r.Errors.Add("Cumulative associations do not match captured unique identities: total="+r.TotalProcesses+" identities="+r.Processes.Count);
      }
      if(Valid(output) && Valid(error)) {
        try { Check(GetFileSizeEx(output,out r.StdoutBytes),"Final stdout size"); Check(GetFileSizeEx(error,out r.StderrBytes),"Final stderr size");
          if(r.Reason=="exit" && (r.StdoutBytes>OutputCap || r.StderrBytes>OutputCap)) r.Reason="output-limit";
        } catch(Exception ex) { r.Errors.Add(ex.Message); }
      }
      for(int i=owned.Count-1;i>=0;i--) { if(Valid(owned[i])) CloseOwned(owned,owned[i],r); }
      if(attrsReady) DeleteProcThreadAttributeList(attrs);
      if(handles!=IntPtr.Zero) Marshal.FreeHGlobal(handles);
      if(attrs!=IntPtr.Zero) Marshal.FreeHGlobal(attrs);
      if(cleanup.ElapsedMilliseconds>=5000) r.Errors.Add("Cleanup budget exhausted including handle closure");
      r.CleanupMilliseconds=cleanup.ElapsedMilliseconds;
    }
    // Files are local owned regular files. No reader exists during the process lifetime.
    if(r.RootSignaled && r.JobEmpty && r.WritersRetired && r.Errors.Count==0 && r.Reason!="output-limit" && r.OwnedHandles==r.ClosedHandles && cleanup.ElapsedMilliseconds<5000) {
      try { r.Output=Snapshot(stdout,r); r.Error=Snapshot(stderr,r); r.OutputRead=true; r.ExclusiveSnapshot=true; }
      catch(Exception ex) { r.SnapshotWin32Error=ex.HResult & 65535; r.Errors.Add("Exclusive output retirement proof Win32="+r.SnapshotWin32Error+": "+ex.Message); }
    }
    if(protocol!=null) r.LimitChildRan=File.Exists(Path.Combine(protocol,"limit-child-ran"));
    try { if(stdout!=null && File.Exists(stdout)) File.Delete(stdout); if(stderr!=null && File.Exists(stderr)) File.Delete(stderr);
      if(protocol!=null) { foreach(string file in Directory.GetFiles(protocol)) File.Delete(file); Directory.Delete(protocol); }
      if(dir!=null) Directory.Delete(dir); }
    catch(Exception ex) { r.Errors.Add("Owned temporary file cleanup: "+ex.Message); }
    r.CleanupMilliseconds=cleanup.ElapsedMilliseconds;
    if(r.CleanupMilliseconds>=5000) r.Errors.Add("Cleanup budget exhausted including output/file cleanup");
    return r;
  }
}
