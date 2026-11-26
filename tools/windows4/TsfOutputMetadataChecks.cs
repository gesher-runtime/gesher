// CI-only real file-object synchronization controls for TsfJobSupervisor.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;

public static class TsfOutputMetadataChecks {
  [StructLayout(LayoutKind.Sequential)] struct SA { public int size; public IntPtr descriptor; public int inherit; }
  [StructLayout(LayoutKind.Sequential)] struct Overlapped {
    public UIntPtr internalValue, internalHigh;
    public uint offset, offsetHigh;
    public IntPtr eventHandle;
  }
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern IntPtr CreateFile(string path,uint access,uint share,ref SA sa,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr ReOpenFile(IntPtr file,uint access,uint share,uint flags);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool LockFileEx(IntPtr file,uint flags,uint reserved,uint low,uint high,ref Overlapped overlapped);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool UnlockFileEx(IntPtr file,uint reserved,uint low,uint high,ref Overlapped overlapped);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileSizeEx(IntPtr file,out long size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetHandleInformation(IntPtr handle,out uint flags);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteFile(IntPtr file,byte[] bytes,uint count,out uint written,IntPtr overlapped);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DuplicateHandle(IntPtr sourceProcess,IntPtr source,IntPtr targetProcess,out IntPtr target,uint access,bool inherit,uint options);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenThread(uint access,bool inherit,uint id);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetThreadIOPendingFlag(IntPtr thread,out bool pending);

  // On an unexpected native retirement failure, never close storage/handles
  // still referenced by a worker. Keep the failed case owned until CI exits.
  static readonly List<Case> Quarantined=new List<Case>();
  sealed class Case {
    public IntPtr writer, holder, observer, writerThread;
    public Thread blocked, query;
    public bool writerStarted, readerStarted;
    public readonly ManualResetEvent writerReady=new ManualResetEvent(false);
    public readonly ManualResetEvent queryReady=new ManualResetEvent(false);
    public readonly ManualResetEvent queryDone=new ManualResetEvent(false);
    public Exception writerError, queryError;
    public long observedSize=-1;
    public bool holderLocked, fileCreated;
    public Overlapped held;
  }
  static bool Valid(IntPtr h) { return h!=IntPtr.Zero && h!=new IntPtr(-1); }
  static void Check(bool condition,string operation) {
    if(!condition) throw new Win32Exception(Marshal.GetLastWin32Error(),operation);
  }
  static void Require(bool condition,string message) {
    if(!condition) throw new IOException(message);
  }
  static IntPtr Observer(IntPtr writer) {
    MethodInfo method=typeof(TsfJobSupervisor).GetMethod("OpenSizeObserver",BindingFlags.Static|BindingFlags.NonPublic);
    Require(method!=null,"Actual controller observer helper missing");
    try { return (IntPtr)method.Invoke(null,new object[] {writer}); }
    catch(TargetInvocationException e) { throw e.InnerException ?? e; }
  }
  static int Remaining(Stopwatch deadline) { return Math.Max(0,5000-(int)deadline.ElapsedMilliseconds); }
  static void Close(ref IntPtr handle,List<string> errors) {
    if(!Valid(handle)) return;
    if(CloseHandle(handle)) handle=IntPtr.Zero;
    else errors.Add("Close test handle Win32="+Marshal.GetLastWin32Error());
  }

  static void Probe(string path,bool shared) {
    var c=new Case(); Exception primary=null;
    try {
      SA sa=new SA {size=Marshal.SizeOf(typeof(SA)),inherit=1};
      c.writer=CreateFile(path,0x40000000,3,ref sa,1,0x100,IntPtr.Zero);
      Check(Valid(c.writer),"Create actual output writer");
      c.fileCreated=true;
      byte[] bytes=new byte[] {11,22,33,44,55,66,77,88}; uint written;
      Check(WriteFile(c.writer,bytes,(uint)bytes.Length,out written,IntPtr.Zero),"Write actual output bytes");
      Require(written==bytes.Length,"Short control output write");
      c.holder=ReOpenFile(c.writer,0x40000000,3,0);
      Check(Valid(c.holder),"Open independent lock holder");
      if(shared) {
        // Real old ownership/access: a new handle still names the inherited
        // writer's same synchronous file object. Do not request extra rights.
        Check(DuplicateHandle(GetCurrentProcess(),c.writer,GetCurrentProcess(),out c.observer,0,false,2),"Duplicate old shared object");
      } else c.observer=Observer(c.writer);
      uint flags; Check(GetHandleInformation(c.observer,out flags),"Query observer inheritance");
      Require((flags&1)==0,"Size observer is inheritable");
      Check(GetFileSizeEx(c.observer,out c.observedSize),"Read initial actual size");
      Require(c.observedSize==bytes.Length,"Observer does not identify the actual output file");
      if(!shared) {
        bool wrote=WriteFile(c.observer,bytes,1,out written,IntPtr.Zero);
        int writeError=Marshal.GetLastWin32Error();
        Require(!wrote && writeError==5,"Metadata-only observer unexpectedly permits writes");
      }
      Check(LockFileEx(c.holder,3,0,1,0,ref c.held),"Hold actual byte-range lock");
      c.holderLocked=true;
      c.blocked=new Thread(delegate() {
        Overlapped pending=new Overlapped(); bool acquired=false;
        try {
          c.writerThread=OpenThread(0x40,false,GetCurrentThreadId());
          Check(Valid(c.writerThread),"Retain pending writer thread");
          c.writerReady.Set();
          // No FAIL_IMMEDIATELY and no OVERLAPPED file mode: this native I/O
          // remains pending until this test releases the independent holder.
          Check(LockFileEx(c.writer,2,0,1,0,ref pending),"Wait on real synchronous file operation");
          acquired=true;
        } catch(Exception e) { c.writerError=e; }
        finally {
          if(acquired && !UnlockFileEx(c.writer,0,1,0,ref pending))
            c.writerError=new Win32Exception(Marshal.GetLastWin32Error(),"Unlock pending writer");
          c.writerReady.Set();
        }
      });
      c.blocked.IsBackground=true; c.blocked.Start(); c.writerStarted=true;
      var startup=Stopwatch.StartNew();
      Require(c.writerReady.WaitOne(Remaining(startup)),"Writer did not enter native control");
      bool pendingIo=false;
      while(c.writerError==null && c.blocked.IsAlive && Remaining(startup)>0) {
        Check(GetThreadIOPendingFlag(c.writerThread,out pendingIo),"Observe actual pending I/O");
        if(pendingIo) break;
        Thread.Sleep(1);
      }
      Require(pendingIo && c.writerError==null,"Real synchronous writer did not remain pending");
      c.observedSize=-1;
      c.query=new Thread(delegate() {
        try {
          c.queryReady.Set();
          Check(GetFileSizeEx(c.observer,out c.observedSize),"Observe size during pending writer I/O");
        } catch(Exception e) { c.queryError=e; }
        finally { c.queryDone.Set(); }
      });
      c.query.IsBackground=true; c.query.Start(); c.readerStarted=true;
      Require(c.queryReady.WaitOne(5000),"Metadata reader did not enter control");
      // The positive condition is progress while the real writer is still
      // pending, not a comparison of noisy duration measurements. The old
      // shared-object control is released after its bounded observation.
      bool completed=c.queryDone.WaitOne(shared?250:5000);
      Check(GetThreadIOPendingFlag(c.writerThread,out pendingIo),"Confirm writer still pending");
      Require(pendingIo && c.blocked.IsAlive,"Pending writer unexpectedly escaped its held lock");
      Require(completed!=shared,shared?"Old shared-object size read unexpectedly progressed":"Independent size read blocked behind writer I/O");
      if(completed) Require(c.queryError==null && c.observedSize==bytes.Length,"Independent read changed size or failed");
      Console.WriteLine("TSF_SIZE_OBSERVER shared="+shared+" actual_pending_io=true completed_before_release="+completed);
    } catch(Exception e) { primary=e; }
    finally {
      var cleanup=Stopwatch.StartNew(); var errors=new List<string>();
      if(c.holderLocked) {
        if(!UnlockFileEx(c.holder,0,1,0,ref c.held)) errors.Add("Unlock holder Win32="+Marshal.GetLastWin32Error());
        else c.holderLocked=false;
      }
      bool writerJoined=!c.writerStarted || c.blocked.Join(Remaining(cleanup));
      bool readerJoined=!c.readerStarted || c.query.Join(Remaining(cleanup));
      if(!writerJoined || !readerJoined) {
        lock(Quarantined) Quarantined.Add(c);
        errors.Add("Native test retirement unproved; handles/events remain owned");
      } else {
        if(c.writerError!=null) errors.Add("Writer: "+c.writerError.Message);
        if(c.queryError!=null) errors.Add("Reader: "+c.queryError.Message);
        if(c.readerStarted && c.observedSize!=8) errors.Add("Final real size was not preserved");
        Close(ref c.writerThread,errors); Close(ref c.observer,errors);
        Close(ref c.holder,errors); Close(ref c.writer,errors);
        if(Valid(c.writerThread) || Valid(c.observer) || Valid(c.holder) || Valid(c.writer))
          lock(Quarantined) Quarantined.Add(c);
        c.queryDone.Dispose(); c.queryReady.Dispose(); c.writerReady.Dispose();
        if(errors.Count==0 && c.fileCreated) {
          // Same exclusive snapshot contract as the controller: every writer
          // and metadata handle is retired before reading/deleting the file.
          try {
            using(var f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.None))
              Require(f.Length==8,"Exclusive retired output size changed");
            File.Delete(path);
          } catch(Exception e) { errors.Add("Exclusive test cleanup: "+e.Message); }
        }
      }
      if(errors.Count!=0) primary=new IOException((primary==null?"":primary.Message+"; ")+String.Join("; ",errors),primary);
    }
    if(primary!=null) {
      // Preserve the primary/retirement evidence even if outer temporary-file
      // cleanup also fails because a quarantined native owner is still live.
      Console.Error.WriteLine("TSF_SIZE_OBSERVER_FAILURE "+primary);
      System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(primary).Throw();
    }
  }

  public static void Run(string directory) {
    if(Environment.GetEnvironmentVariable("GITHUB_ACTIONS")!="true") throw new InvalidOperationException("CI only");
    bool denied=false;
    try { IntPtr unexpected=Observer(new IntPtr(-1)); if(Valid(unexpected)) Check(CloseHandle(unexpected),"Close unexpected invalid-input observer"); }
    catch(Win32Exception e) { denied=e.NativeErrorCode==6; }
    Require(denied,"Invalid output writer was not rejected with ERROR_INVALID_HANDLE");
    Probe(Path.Combine(directory,"metadata-shared"),true);
    Probe(Path.Combine(directory,"metadata-independent"),false);
    Console.WriteLine("TSF_SIZE_OBSERVER actual_file=true shared_blocked=true independent_progress=true invalid_rejected=true readonly=true noninherited=true retired=true");
  }
}
