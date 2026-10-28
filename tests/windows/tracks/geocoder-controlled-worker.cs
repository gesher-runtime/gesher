using System;
using System.Threading;

// CI fixture only: this owns a real worker, not a Windows Maps provider.
public sealed class ControlledGeocoderWorker {
    readonly ManualResetEvent started = new ManualResetEvent(false);
    readonly ManualResetEvent release = new ManualResetEvent(false);
    readonly ManualResetEvent cancel = new ManualResetEvent(false);
    readonly ManualResetEvent exited = new ManualResetEvent(false);
    readonly Thread worker;
    readonly bool error, blockCancel;
    readonly object sync = new object();
    string status = "Started";
    public int CancelCalls, CancelObserved, SuccessCallbacks, ResultReads, CloseCalls;
    public bool WorkerExited, ResourcesDisposed;
    public ControlledGeocoderWorker(bool error, bool blockCancel) {
        this.error = error; this.blockCancel = blockCancel;
        worker = new Thread(Run); worker.IsBackground = true; worker.Start();
    }
    void Run() {
        try {
            started.Set();
            int signal = WaitHandle.WaitAny(new WaitHandle[] { cancel, release });
            lock (sync) {
                if (signal == 0) { CancelObserved++; status = "Canceled"; }
                else if (error) status = "Error";
                else { status = "Completed"; SuccessCallbacks++; }
            }
        } finally { WorkerExited = true; exited.Set(); }
    }
    public void AwaitStarted() { if (!started.WaitOne(10000)) throw new Exception("worker did not start"); }
    public void Release() { release.Set(); }
    public bool Wait(int ms) { return exited.WaitOne(ms); }
    public string Status { get { lock (sync) return status; } }
    public void Cancel() {
        Interlocked.Increment(ref CancelCalls);
        if (blockCancel) {
            Console.WriteLine("cancel-entered"); Console.Out.Flush();
            new ManualResetEvent(false).WaitOne();
        }
        cancel.Set();
    }
    public string Result() { ResultReads++; return "controlled result"; }
    public void Close() {
        CloseCalls++;
        if (ResourcesDisposed) return;
        if (Status == "Started") throw new Exception("close while started");
        if (!worker.Join(10000)) throw new Exception("worker did not exit");
        started.Dispose(); release.Dispose(); cancel.Dispose(); exited.Dispose();
        ResourcesDisposed = true;
    }
}
