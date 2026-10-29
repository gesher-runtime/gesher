using System;
using System.Threading;
using System.Threading.Tasks;

// CI only. AsyncInfo.Run wraps this supplier in a genuine WinRT async adapter.
// Existing worker/Job tests separately prove actual thread and process retirement.
public sealed class ControlledWinRTSupplier {
    readonly TaskCompletionSource<int> completion = new TaskCompletionSource<int>();
    CancellationTokenRegistration registration;
    public int CancelObserved, CompletionCalls;
    public bool Disposed;
    public Func<CancellationToken, Task<int>> Factory { get { return Start; } }
    Task<int> Start(CancellationToken token) {
        registration = token.Register(() => {
            Interlocked.Increment(ref CancelObserved);
            completion.TrySetCanceled();
        });
        return completion.Task;
    }
    public void Complete(bool error) {
        Interlocked.Increment(ref CompletionCalls);
        if (error) completion.TrySetException(new InvalidOperationException("controlled WinRT error"));
        else completion.TrySetResult(42);
    }
    public void Dispose() { registration.Dispose(); Disposed = true; }
}
