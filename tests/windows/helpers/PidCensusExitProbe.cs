// CI-only observation of actual Windows job PID lists across an owned root's
// exit. The controller and its fail-closed PID validation are unchanged.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

public static class PidCensusExitProbe {
  const uint Signaled=0, Timeout=258;
  static readonly List<IntPtr> Quarantined=new List<IntPtr>();
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct Startup {
    public int size; public string reserved,desktop,title; public uint x,y,dx,dy,xc,yc,fill,flags;
    public ushort show,reservedSize; public IntPtr reservedBytes,input,output,error;
  }
  [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr process,thread; public uint pid,tid; }
  [StructLayout(LayoutKind.Sequential)] struct Limits {
    public long processTime,jobTime; public uint flags; public UIntPtr min,max;
    public uint active; public UIntPtr affinity; public uint priority,scheduling;
  }
  [StructLayout(LayoutKind.Sequential)] struct Io { public ulong a,b,c,d,e,f; }
  [StructLayout(LayoutKind.Sequential)] struct Extended { public Limits basic; public Io io; public UIntPtr pm,jm,pp,pj; }
  [StructLayout(LayoutKind.Sequential)] struct Accounting {
    public long user,kernel,periodUser,periodKernel; public uint faults,total,active,terminated;
  }
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr sa,string name);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int cls,ref Extended info,uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int cls,out Accounting info,uint size,IntPtr length);
  [DllImport("kernel32.dll",SetLastError=true,EntryPoint="QueryInformationJobObject")] static extern bool QueryPidList(IntPtr job,int cls,IntPtr list,uint size,IntPtr length);
  [DllImport("kernel32.dll",SetLastError=true,EntryPoint="QueryInformationJobObject")] static extern bool QueryLimits(IntPtr job,int cls,out Extended info,uint size,IntPtr length);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool IsProcessInJob(IntPtr process,IntPtr job,out bool member);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle,uint milliseconds);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr process,out uint code);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr process,out long birth,out long exit,out long kernel,out long user);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateJobObject(IntPtr job,uint code);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr process,uint code);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode,EntryPoint="CreateProcessW")]
  static extern bool CreateProcess(string app,StringBuilder command,IntPtr psa,IntPtr tsa,
    bool inherit,uint flags,IntPtr environment,string cwd,ref Startup startup,out ProcessInfo process);

  public sealed class Snapshot {
    public int Assigned,Listed; public uint RootWait;
    public List<long> Pids=new List<long>();
    public bool Complete { get { return Assigned==Listed && Listed>=0 && Listed<=64; } }
  }
  public sealed class Observation {
    public int Iteration,Queries,ControllerCalls,ControllerIncomplete;
    public uint Pid,ExitCode,Total,FinalActive;
    public long Birth,RootHandle,JobHandle,Milliseconds;
    public bool Assigned,Member,RootSignaled,ThreadClosed,ProcessClosed,JobClosed;
    public Snapshot Suspended,DuringIncomplete,Retained,RetainedAfterController,Closed;
    public List<string> Errors=new List<string>();
  }
  static bool Valid(IntPtr h) { return h!=IntPtr.Zero && h!=new IntPtr(-1); }
  static void Check(bool value,string name) {
    if(!value) { int error=Marshal.GetLastWin32Error(); throw new Win32Exception(error,name+" Win32="+error); }
  }
  static uint Remaining(Stopwatch clock) { return (uint)Math.Max(0,5000-clock.ElapsedMilliseconds); }
  static Snapshot Read(IntPtr job,IntPtr root) {
    const int capacity=64; int bytes=8+capacity*IntPtr.Size;
    IntPtr memory=Marshal.AllocHGlobal(bytes);
    try {
      Check(QueryPidList(job,3,memory,(uint)bytes,IntPtr.Zero),"Probe PID list");
      var result=new Snapshot {Assigned=Marshal.ReadInt32(memory),Listed=Marshal.ReadInt32(memory,4),
        RootWait=Valid(root)?WaitForSingleObject(root,0):UInt32.MaxValue};
      if(Valid(root) && result.RootWait!=Signaled && result.RootWait!=Timeout)
        throw new IOException("Probe retained-root wait failed: "+result.RootWait);
      if(result.Listed<0 || result.Listed>capacity) throw new IOException("Malformed probe PID count: "+result.Listed);
      for(int i=0;i<result.Listed;i++) result.Pids.Add(Marshal.ReadIntPtr(memory,8+i*IntPtr.Size).ToInt64());
      return result;
    } finally { Marshal.FreeHGlobal(memory); }
  }
  static void ActualController(MethodInfo method,IntPtr job,Observation observation) {
    observation.ControllerCalls++;
    try { method.Invoke(null,new object[] {job}); }
    catch(TargetInvocationException e) {
      if(e.InnerException is IOException && e.InnerException.Message=="Incomplete job PID census") {
        observation.ControllerIncomplete++; return;
      }
      throw;
    }
  }
  static void Close(ref IntPtr handle,Observation observation,string role) {
    if(!Valid(handle)) return;
    if(!CloseHandle(handle)) {
      int error=Marshal.GetLastWin32Error(); observation.Errors.Add("Probe close "+role+" Win32="+error);
      lock(Quarantined) { Quarantined.Add(handle); }
    } else {
      if(role=="thread") observation.ThreadClosed=true;
      if(role=="process") observation.ProcessClosed=true;
      if(role=="job") observation.JobClosed=true;
    }
    handle=IntPtr.Zero;
  }
  public static Observation Run(Type controller,string commandInterpreter,int iteration) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    if(controller==null || controller.FullName!="TsfJobSupervisor") throw new IOException("Actual controller type missing");
    var method=controller.GetMethod("Pids",BindingFlags.NonPublic|BindingFlags.Static);
    if(method==null || method.ReturnType!=typeof(List<uint>)) throw new IOException("Actual controller PID entry changed");
    var result=new Observation {Iteration=iteration};
    IntPtr job=IntPtr.Zero,process=IntPtr.Zero,thread=IntPtr.Zero;
    var clock=Stopwatch.StartNew();
    try {
      job=CreateJobObject(IntPtr.Zero,null); Check(Valid(job),"Probe create job"); result.JobHandle=job.ToInt64();
      Extended limits=new Extended(); limits.basic.flags=0x2000|0x8; limits.basic.active=1;
      Check(SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf(typeof(Extended))),"Probe set limits");
      Extended actual;
      Check(QueryLimits(job,9,out actual,(uint)Marshal.SizeOf(typeof(Extended)),IntPtr.Zero),"Probe read limits");
      if((actual.basic.flags&(0x2000|0x8))!=(0x2000|0x8) || actual.basic.active!=1)
        throw new IOException("Probe limits were not enforced");
      string exe=Path.GetFullPath(commandInterpreter);
      if(!String.Equals(Path.GetFileName(exe),"cmd.exe",StringComparison.OrdinalIgnoreCase))
        throw new IOException("Probe requires the actual Windows command interpreter");
      Startup startup=new Startup(); startup.size=Marshal.SizeOf(typeof(Startup)); ProcessInfo child;
      // No inherited handles. Detached prevents a new console-host association;
      // the child is assigned while its only thread is still suspended.
      Check(CreateProcess(exe,new StringBuilder("\""+exe+"\" /d /c exit 0"),IntPtr.Zero,IntPtr.Zero,
        false,0xcu,IntPtr.Zero,null,ref startup,out child),"Probe create suspended root");
      process=child.process; thread=child.thread; result.Pid=child.pid; result.RootHandle=process.ToInt64();
      long exit,kernel,user; Check(GetProcessTimes(process,out result.Birth,out exit,out kernel,out user),"Probe birth");
      Check(AssignProcessToJobObject(job,process),"Probe assignment"); result.Assigned=true;
      bool member; Check(IsProcessInJob(process,job,out member),"Probe membership"); result.Member=member;
      Accounting before; Check(QueryInformationJobObject(job,1,out before,(uint)Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero),"Probe initial accounting");
      result.Suspended=Read(job,process);
      if(!member || before.total!=1 || before.active!=1 || !result.Suspended.Complete ||
         result.Suspended.Listed!=1 || result.Suspended.Pids[0]!=child.pid || result.Suspended.RootWait!=Timeout)
        throw new IOException("Probe suspended root identity incomplete");
      ActualController(method,job,result);
      if(result.ControllerIncomplete!=0) throw new IOException("Controller rejected suspended root");
      if(ResumeThread(thread)!=1) throw new IOException("Probe suspension count mismatch");
      while(WaitForSingleObject(process,0)==Timeout && Remaining(clock)>0) {
        Snapshot observed=Read(job,process); result.Queries++;
        if(!observed.Complete && result.DuringIncomplete==null) result.DuringIncomplete=observed;
        ActualController(method,job,result);
      }
      uint waited=WaitForSingleObject(process,Remaining(clock));
      if(waited!=Signaled) throw new IOException("Probe root did not retire within shared 5000ms");
      result.RootSignaled=true;
      Check(GetExitCodeProcess(process,out result.ExitCode),"Probe exit code");
      if(result.ExitCode!=0) throw new IOException("Probe command failed: "+result.ExitCode);
      long birth; Check(GetProcessTimes(process,out birth,out exit,out kernel,out user),"Probe retained identity");
      if(birth!=result.Birth || result.Birth<=0) throw new IOException("Probe retained identity changed");
      result.Retained=Read(job,process);
      ActualController(method,job,result);
      result.RetainedAfterController=Read(job,process);
    } catch(Exception e) { result.Errors.Add(e.ToString()); }
    finally {
      if(Valid(process) && !result.RootSignaled) {
        try {
          uint state=WaitForSingleObject(process,0);
          if(state!=Signaled) {
            Check(result.Assigned?TerminateJobObject(job,124):TerminateProcess(process,124),"Probe terminate owned root");
            state=WaitForSingleObject(process,Remaining(clock));
          }
          result.RootSignaled=state==Signaled;
          if(!result.RootSignaled) result.Errors.Add("Probe root retirement unproved within shared 5000ms");
        } catch(Exception e) { result.Errors.Add(e.ToString()); }
      }
      if(result.RootSignaled || !Valid(process)) {
        Close(ref thread,result,"thread"); Close(ref process,result,"process");
      } else {
        lock(Quarantined) { if(Valid(thread)) Quarantined.Add(thread); if(Valid(process)) Quarantined.Add(process); }
        thread=process=IntPtr.Zero;
      }
      if(Valid(job) && result.Assigned) {
        try {
          Accounting final;
          do {
            Check(QueryInformationJobObject(job,1,out final,(uint)Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero),"Probe final accounting");
            if(final.active==0 || Remaining(clock)==0) break;
            System.Threading.Thread.Sleep((int)Math.Min(10,Remaining(clock)));
          } while(true);
          result.Total=final.total; result.FinalActive=final.active;
          result.Closed=Read(job,IntPtr.Zero);
          if(final.active!=0 || final.total!=1 || !result.Closed.Complete || result.Closed.Listed!=0)
            result.Errors.Add("Probe final job identity or retirement incomplete");
        } catch(Exception e) { result.Errors.Add(e.ToString()); }
      }
      Close(ref job,result,"job"); result.Milliseconds=clock.ElapsedMilliseconds;
    }
    return result;
  }
}
