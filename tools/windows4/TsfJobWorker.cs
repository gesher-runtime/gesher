// CI-only bounded-output controls. Descendants inherit the containing job and file handles.
using System;
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
  static int Main(string[] args) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true" || args.Length!=1) return 91;
    Console.WriteLine("CONTROL "+args[0]); Console.Error.WriteLine("CONTROL stderr");
    switch(args[0]) {
      case "normal": return 0;
      case "exit23": return 23;
      case "hang": Thread.Sleep(120000); return 92;
      case "writer": Console.WriteLine("RETAINED_WRITER_READY"); Console.Out.Flush(); Thread.Sleep(120000); return 93;
      case "descendant":
        SI si=new SI(); si.size=Marshal.SizeOf(typeof(SI)); si.flags=0x100;
        si.input=GetStdHandle(-10); si.output=GetStdHandle(-11); si.error=GetStdHandle(-12);
        string exe=typeof(TsfJobWorker).Assembly.Location; PI child;
        if(!CreateProcess(exe,new StringBuilder("\""+exe+"\" writer"),IntPtr.Zero,IntPtr.Zero,true,0x08000000,IntPtr.Zero,null,ref si,out child)) return 94;
        Console.WriteLine("DESCENDANT_PID="+child.pid);
        bool closed=CloseHandle(child.thread); closed=CloseHandle(child.process)&&closed;
        Console.Out.Flush(); return closed?0:95;
      case "output": Console.Write(new string('x',1100000)); Console.Out.Flush(); Thread.Sleep(120000); return 96;
      default: return 97;
    }
  }
}
