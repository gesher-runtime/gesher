// Source only locally. Compiled and executed exclusively by Windows CI.
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Runtime.InteropServices;
using System.Reflection;
using System.Text;

public class Graphics84 {
  static int Main(string[] args) {
    if (Environment.GetEnvironmentVariable("GITHUB_ACTIONS") != "true") return 90;
    if (args.Length == 2 && args[0] == "hold") { Thread.Sleep(60000); return 0; }
    string mode = Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_MODE");
    string receipt = Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_RECEIPT");
    string[] expected = File.ReadAllLines(Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_ARGS"));
    if (args.Length != expected.Length) return 91;
    for (int i = 0; i < args.Length; ++i) if (args[i] != expected[i]) return 92;
    if (Environment.CurrentDirectory != Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_CWD")) return 93;
    if (Environment.GetEnvironmentVariable("GESHER_TRACE_FAULTS") != "1") return 94;
    int selfPid;
    using (var self = Process.GetCurrentProcess()) {
      selfPid=self.Id;
      string parentJson="{\"schema\":\"GRAPHICS84_PARENT_V1\",\"source\":"+Q(Environment.GetEnvironmentVariable("GITHUB_SHA"))+
        ",\"case\":"+Q(mode)+",\"nonce\":"+Q(Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_NONCE"))+
        ",\"pid\":"+self.Id+",\"creation_filetime\":"+Q(Birth(GetCurrentProcess()).ToString())+
        ",\"expected_image\":"+Q(self.MainModule.FileName)+",\"cwd\":"+Q(Environment.CurrentDirectory)+",\"argv_env\":\"EXACT\"}";
      if(Encoding.UTF8.GetByteCount(parentJson)>16384) return 95;
      File.WriteAllText(receipt,parentJson,new UTF8Encoding(false,true));
    }
    Console.Error.WriteLine("graphics84 actual stderr " + mode);
    if (mode == "timeout") { Thread.Sleep(300000); return 0; }
    if (mode != "missing-marker") Console.WriteLine("GraphicsFixture all passed");
    Console.WriteLine("PASS  graphics84 actual fixture " + mode);
    if (mode == "retained") {
      return Transfer(receipt + ".child",selfPid);

    }
    return mode == "nonzero" ? 23 : 0;
  }
  // Same source is compiled as the CI-only controller bridge. No local import.
  [StructLayout(LayoutKind.Sequential)] public struct FT { public uint Low, High; public ulong Raw { get { return ((ulong)High << 32) | Low; } } }
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint rights, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);
  [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int which);
  [DllImport("kernel32.dll", SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
  public static extern bool DuplicateHandle(IntPtr source, IntPtr handle, IntPtr target, out IntPtr result, uint rights, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint options);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern uint GetProcessId(IntPtr process);
  [DllImport("kernel32.dll", SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
  public static extern bool GetProcessTimes(IntPtr process, out FT creation, out FT exit, out FT kernel, out FT user);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr process, uint ms);
  [DllImport("kernel32.dll", SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool TerminateProcess(IntPtr process, uint code);
  [DllImport("kernel32.dll", SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool GetHandleInformation(IntPtr handle, out uint flags);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
  static extern bool CreateProcess(string app, StringBuilder command, IntPtr pa, IntPtr ta, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint flags, IntPtr env, string cwd, ref Process84.Startup startup, out Process84.ProcessInfo info);
  public static ulong Birth(IntPtr h) {
    FT c,e,k,u; bool ok=GetProcessTimes(h,out c,out e,out k,out u); int error=Marshal.GetLastWin32Error();
    if (!ok) throw new InvalidOperationException("creation FILETIME unavailable:"+error);
    return c.Raw;
  }
  public sealed class Slot { public IntPtr Handle; public string State="UNATTEMPTED"; }
  public static void Close(Slot s) {
    if (s.State=="DONE") return;
    if (s.State!="OWNED") throw new InvalidOperationException("uncertain close retained");
    s.State="FAILED";
    bool ok=CloseHandle(s.Handle); int closeError=Marshal.GetLastWin32Error();
    if (!ok) throw new InvalidOperationException("close failed:"+closeError);
    uint flags; bool valid=GetHandleInformation(s.Handle,out flags); int probeError=Marshal.GetLastWin32Error();
    if (valid || probeError!=6) throw new InvalidOperationException("close native-invalidity unproved");
    s.State="DONE";
  }
  static string Q(string s) {
    foreach(char c in s) if(c<32) throw new InvalidOperationException("control character in receipt");
    return "\""+s.Replace("\\","\\\\").Replace("\"","\\\"")+"\"";
  }
  static bool Hex(string s,int n) {
    if(s==null || s.Length!=n) return false;
    foreach(char c in s) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    return true;
  }
  static int Transfer(string receipt,int selfPid) {
    // Slots/root exist before any native attempt. Remote handle is NEVER local.
    var output=new Slot(); var error=new Slot(); var child=new Slot(); var thread=new Slot(); var target=new Slot();
    IntPtr remote=IntPtr.Zero; Process84.ProcessInfo info=new Process84.ProcessInfo();
    bool published=false; bool duplicateUnknown=false; bool createdUnknown=false;
    uint ownerPid=0; ulong ownerBirth=0,childBirth=0,selfBirth=0;
    string source=Environment.GetEnvironmentVariable("GITHUB_SHA"), nonce=Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_NONCE");
    string expected=Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_EXPECTED_IMAGE");
    string ownerPidText=Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_OWNER_PID");
    string ownerBirthText=Environment.GetEnvironmentVariable("GESHER_GRAPHICS84_OWNER_FILETIME");
    while(true) {
      try {
        if(IntPtr.Size!=8 || !Hex(source,40) || !Hex(nonce,64) || !UInt32.TryParse(ownerPidText,out ownerPid) || ownerPid==0 || ownerPid.ToString()!=ownerPidText || !UInt64.TryParse(ownerBirthText,out ownerBirth) || ownerBirth.ToString()!=ownerBirthText || expected!=Assembly.GetExecutingAssembly().Location) throw new InvalidOperationException("fixture-only input rejected");
        selfBirth=Birth(GetCurrentProcess());
        output.State="FAILED";
        if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(-11),GetCurrentProcess(),out output.Handle,0,true,2)) { int e=Marshal.GetLastWin32Error(); throw new InvalidOperationException("stdout duplicate:"+e); }
        output.State="OWNED";
        error.State="FAILED";
        if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(-12),GetCurrentProcess(),out error.Handle,0,true,2)) { int e=Marshal.GetLastWin32Error(); throw new InvalidOperationException("stderr duplicate:"+e); }
        error.State="OWNED";
        var startup=new Process84.Startup(); startup.cb=Marshal.SizeOf(typeof(Process84.Startup)); startup.flags=0x100; startup.output=output.Handle; startup.error=error.Handle;
        createdUnknown=true;
        bool created=CreateProcess(expected,new StringBuilder("\""+expected+"\" hold unused"),IntPtr.Zero,IntPtr.Zero,true,0x08000000,IntPtr.Zero,null,ref startup,out info);
        int createError=Marshal.GetLastWin32Error();
        child.Handle=info.process; thread.Handle=info.thread;
        if(!created) throw new InvalidOperationException("child acquisition uncertain:"+createError);
        child.State="OWNED"; thread.State="OWNED"; createdUnknown=false;
        childBirth=Birth(child.Handle);
        target.State="FAILED";
        target.Handle=OpenProcess(0x1040,false,ownerPid); int openError=Marshal.GetLastWin32Error();
        if(target.Handle==IntPtr.Zero) throw new InvalidOperationException("owner target unavailable:"+openError);
        target.State="OWNED";
        if(Birth(target.Handle)!=ownerBirth) throw new InvalidOperationException("original pwsh birth mismatch");
        duplicateUnknown=true;
        bool duplicated=DuplicateHandle(GetCurrentProcess(),child.Handle,target.Handle,out remote,0x101001,false,0);
        int duplicateError=Marshal.GetLastWin32Error();
        if(!duplicated) throw new InvalidOperationException("target child duplicate uncertain:"+duplicateError);
        duplicateUnknown=false;
        if(remote==IntPtr.Zero || remote.ToInt64()==-1 || remote.ToInt64()==-2) throw new InvalidOperationException("invalid returned target value");
        Close(thread); Close(output); Close(error); Close(target);
        string json="{\"schema\":\"GRAPHICS84_HANDLE_TRANSFER_V1\",\"source\":"+Q(source)+",\"case\":\"retained\",\"nonce\":"+Q(nonce)+",\"bits\":64,\"parent_pid\":"+ownerPid+",\"parent_creation_filetime\":"+Q(ownerBirth.ToString())+",\"fixture_pid\":"+selfPid+",\"fixture_creation_filetime\":"+Q(selfBirth.ToString())+",\"child_pid\":"+info.pid+",\"child_creation_filetime\":"+Q(childBirth.ToString())+",\"destination_handle_hex16\":"+Q(unchecked((ulong)remote.ToInt64()).ToString("x16"))+",\"duplicate_state\":\"RETURNED_OWNED_TARGET\",\"expected_create_application_path\":"+Q(expected)+",\"parent_access\":\"0x1040\",\"child_access\":\"0x101001\",\"inherit\":false,\"options\":0,\"publication_phase\":\"TRANSFER_PUBLISHED\",\"source_prepublication_releases\":{\"thread\":\"DONE\",\"output\":\"DONE\",\"error\":\"DONE\",\"target\":\"DONE\"},\"source_child_escrow\":\"REQUIRES_EXACT_PARENT_NATIVE_EXIT0\"}";
        if(Encoding.UTF8.GetByteCount(json)>16384) throw new InvalidOperationException("oversize receipt");
        File.WriteAllText(receipt,json,new UTF8Encoding(false,true)); published=true;
        Close(child); // ONLY after publication returned; unknown close never requery.
        return 0;
      } catch {
        // NO replay of any acquisitions/releases. Exact callable source escrow
        // may only wait, never terminate/revoke receiver; unknown slot parks.
        while(true) {
          try {
            if(!createdUnknown && !duplicateUnknown && child.State=="OWNED" && !published) {
              uint w=WaitForSingleObject(child.Handle,5000); int e=Marshal.GetLastWin32Error();
              if(w!=0 && w!=258) { child.State="FAILED"; if(e==0) duplicateUnknown=true; }
            }
            // Revisit the ORIGINAL reference ledger, never uncertain handles.
            if(output.State==null || error.State==null || thread.State==null || target.State==null || remote==IntPtr.Zero) duplicateUnknown=true;
            Thread.Sleep(5000);
          } catch { try { Thread.Sleep(5000); } catch {} }
        }
      }
    }
  }
}
