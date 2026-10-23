package android.os;

import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/** Stand-in for the framework class (the same behaviour). */
public class HandlerExecutor implements Executor {
  private final Handler mHandler;

  public HandlerExecutor(Handler handler) {
    mHandler = handler;
  }

  @Override
  public void execute(Runnable command) {
    if (!mHandler.post(command)) {
      throw new RejectedExecutionException(mHandler + " is shutting down");
    }
  }
}
