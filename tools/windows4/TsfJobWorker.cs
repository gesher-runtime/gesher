// CI-only bounded-output controls. Descendants inherit the containing job and file handles.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
class TsfJobWorker {
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct SI {
    public int size; public string reserved,desktop,title; public uint x,y,dx,dy,xc,yc,fill,flags;
    public ushort show,reservedSize; public IntPtr reservedBytes,input,output,error;
  }
  [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr process,thread; public uint pid,tid; }
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool CreateProcess(string app,StringBuilder command,IntPtr p,IntPtr t,bool inherit,uint flags,IntPtr env,string cwd,ref SI start,out PI child);
  [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int which);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr h);
  static void Publish(string directory,string name,string value) {
    string temporary=Path.Combine(directory,name+".tmp"); File.WriteAllText(temporary,value);
    File.Move(temporary,Path.Combine(directory,name));
  }
  static void Gate(string directory,string name) {
    // Controller's absolute main/job deadline owns cancellation. Never accept a failed gate as readiness.
    for(int i=0;i<12000;i++) { if(File.Exists(Path.Combine(directory,name))) return; Thread.Sleep(10); }
    throw new IOException("Controller barrier missing: "+name);
  }
  static int Main(string[] args) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true" || args.Length!=2) return 91;
    string protocol=args[1];
    Console.WriteLine("CONTROL "+args[0]); Console.Error.WriteLine("CONTROL stderr");
    Console.Out.Flush(); Console.Error.Flush();
    if(args[0]!="writer" && args[0]!="race-writer") { Publish(protocol,"started","ready"); Gate(protocol,"start-go"); }
    switch(args[0]) {
      case "normal": return 0;
      case "exit23": return 23;
      case "hang": Thread.Sleep(120000); return 92;
      case "writer":
        Console.WriteLine("RETAINED_WRITER_READY"); Console.Out.Flush();
        Publish(protocol,"writer-ready","flushed"); Thread.Sleep(120000); return 93;
      case "race-writer": Gate(protocol,"race-go"); return 0;
      case "race":
        SI raceStart=new SI(); raceStart.size=Marshal.SizeOf(typeof(SI)); raceStart.flags=0x100;
        raceStart.input=GetStdHandle(-10); raceStart.output=GetStdHandle(-11); raceStart.error=GetStdHandle(-12);
        string raceExe=typeof(TsfJobWorker).Assembly.Location; PI raceChild;
        if(!CreateProcess(raceExe,new StringBuilder("\""+raceExe+"\" race-writer \""+protocol+"\""),IntPtr.Zero,IntPtr.Zero,true,0x08000000,IntPtr.Zero,null,ref raceStart,out raceChild)) return 94;
        Publish(protocol,"race-child",raceChild.pid.ToString()); Gate(protocol,"race-done");
        bool raceClosed=CloseHandle(raceChild.thread); raceClosed=CloseHandle(raceChild.process)&&raceClosed;
        return raceClosed?0:95;
      case "descendant":
      case "unknown":
        SI si=new SI(); si.size=Marshal.SizeOf(typeof(SI)); si.flags=0x100;
        si.input=GetStdHandle(-10); si.output=GetStdHandle(-11); si.error=GetStdHandle(-12);
        string exe=typeof(TsfJobWorker).Assembly.Location; PI child;
        if(!CreateProcess(exe,new StringBuilder("\""+exe+"\" writer \""+protocol+"\""),IntPtr.Zero,IntPtr.Zero,true,0x08000004,IntPtr.Zero,null,ref si,out child)) return 94;
        if(args[0]=="unknown") {
          Publish(protocol,"unknown",child.pid.ToString()); Gate(protocol,"unknown-go");
          CloseHandle(child.thread); CloseHandle(child.process); return 99;
        }
        Publish(protocol,"child",child.pid.ToString()); Gate(protocol,"child-go");
        if(ResumeThread(child.thread)!=1) return 98;
        Gate(protocol,"writer-ready"); Gate(protocol,"writer-go");
        Console.WriteLine("DESCENDANT_PID="+child.pid);
        bool closed=CloseHandle(child.thread); closed=CloseHandle(child.process)&&closed;
        Console.Out.Flush(); return closed?0:95;
      case "output": Console.Write(new string('x',1100000)); Console.Out.Flush(); Thread.Sleep(120000); return 96;
      default: return 97;
    }
  }
}
