/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.tasks;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

public final class Tasks {
    private Tasks() {
    }

    public static <TResult> Task<TResult> forResult(TResult result) {
        TaskImpl<TResult> task = new TaskImpl<>();
        task.setResult(result);
        return task;
    }

    public static <TResult> Task<TResult> forException(Exception e) {
        TaskImpl<TResult> task = new TaskImpl<>();
        task.setException(e);
        return task;
    }

    public static <TResult> Task<TResult> forCanceled() {
        TaskImpl<TResult> task = new TaskImpl<>();
        task.cancel();
        return task;
    }

    public static <TResult> TResult await(Task<TResult> task)
            throws ExecutionException, InterruptedException {
        if (task.isComplete()) {
            return getResultOrThrow(task);
        }
        CountDownLatch latch = new CountDownLatch(1);
        task.addOnCompleteListener(t -> latch.countDown());
        latch.await();
        return getResultOrThrow(task);
    }

    public static <TResult> TResult await(Task<TResult> task, long timeout, TimeUnit unit)
            throws ExecutionException, InterruptedException, TimeoutException {
        if (task.isComplete()) {
            return getResultOrThrow(task);
        }
        CountDownLatch latch = new CountDownLatch(1);
        task.addOnCompleteListener(t -> latch.countDown());
        if (!latch.await(timeout, unit)) {
            throw new TimeoutException("Timed out waiting for Task");
        }
        return getResultOrThrow(task);
    }

    public static Task<Void> whenAll(Task<?>... tasks) {
        return whenAll(Arrays.asList(tasks));
    }

    public static Task<Void> whenAll(Collection<? extends Task<?>> tasks) {
        if (tasks == null || tasks.isEmpty()) {
            return forResult(null);
        }
        TaskCompletionSource<Void> tcs = new TaskCompletionSource<>();
        int count = tasks.size();
        int[] remaining = new int[] {count};
        for (Task<?> t : tasks) {
            t.addOnCompleteListener(completed -> {
                synchronized (remaining) {
                    if (completed.getException() != null) {
                        tcs.trySetException(completed.getException());
                    } else if (remaining[0] > 0 && --remaining[0] == 0) {
                        tcs.trySetResult(null);
                    }
                }
            });
        }
        return tcs.getTask();
    }

    public static <TResult> Task<List<TResult>> whenAllSuccess(
            Collection<? extends Task<?>> tasks) {
        if (tasks == null || tasks.isEmpty()) {
            return forResult(new ArrayList<>());
        }
        TaskCompletionSource<List<TResult>> tcs = new TaskCompletionSource<>();
        whenAll(tasks).addOnCompleteListener(all -> {
            if (all.isSuccessful()) {
                List<TResult> results = new ArrayList<>();
                for (Task<?> t : tasks) {
                    @SuppressWarnings("unchecked")
                    TResult res = (TResult) t.getResult();
                    results.add(res);
                }
                tcs.setResult(results);
            } else if (all.getException() != null) {
                tcs.setException(all.getException());
            }
        });
        return tcs.getTask();
    }

    private static <TResult> TResult getResultOrThrow(Task<TResult> task)
            throws ExecutionException {
        if (task.isSuccessful()) {
            return task.getResult();
        }
        if (task.isCanceled()) {
            throw new CancellationException("Task is already canceled");
        }
        throw new ExecutionException(task.getException());
    }

    static final class TaskImpl<TResult> extends Task<TResult> {
        private final Object mLock = new Object();
        private boolean mComplete;
        private boolean mCanceled;
        private TResult mResult;
        private Exception mException;

        private final List<Runnable> mCallbacks = new ArrayList<>();

        @Override
        public boolean isComplete() {
            synchronized (mLock) {
                return mComplete;
            }
        }

        @Override
        public boolean isSuccessful() {
            synchronized (mLock) {
                return mComplete && !mCanceled && mException == null;
            }
        }

        @Override
        public boolean isCanceled() {
            synchronized (mLock) {
                return mCanceled;
            }
        }

        @Override
        public TResult getResult() {
            synchronized (mLock) {
                if (!mComplete) {
                    throw new IllegalStateException("Task is not yet complete");
                }
                if (mCanceled) {
                    throw new CancellationException("Task is already canceled.");
                }
                if (mException != null) {
                    throw new RuntimeExecutionException(mException);
                }
                return mResult;
            }
        }

        @Override
        public <X extends Throwable> TResult getResult(Class<X> exceptionType) throws X {
            synchronized (mLock) {
                if (!mComplete) {
                    throw new IllegalStateException("Task is not yet complete");
                }
                if (mCanceled) {
                    throw new CancellationException("Task is already canceled.");
                }
                if (exceptionType.isInstance(mException)) {
                    throw exceptionType.cast(mException);
                }
                if (mException != null) {
                    throw new RuntimeExecutionException(mException);
                }
                return mResult;
            }
        }

        @Override
        public Exception getException() {
            synchronized (mLock) {
                return mException;
            }
        }

        @Override
        public Task<TResult> addOnSuccessListener(OnSuccessListener<? super TResult> listener) {
            return addOnSuccessListener(DirectExecutor.INSTANCE, listener);
        }

        @Override
        public Task<TResult> addOnSuccessListener(Executor executor,
                OnSuccessListener<? super TResult> listener) {
            registerCallback(() -> {
                if (isSuccessful()) {
                    executor.execute(() -> listener.onSuccess(getResult()));
                }
            });
            return this;
        }

        @Override
        public Task<TResult> addOnFailureListener(OnFailureListener listener) {
            return addOnFailureListener(DirectExecutor.INSTANCE, listener);
        }

        @Override
        public Task<TResult> addOnFailureListener(Executor executor,
                OnFailureListener listener) {
            registerCallback(() -> {
                if (!isSuccessful() && !isCanceled()) {
                    executor.execute(() -> listener.onFailure(getException()));
                }
            });
            return this;
        }

        @Override
        public Task<TResult> addOnCompleteListener(OnCompleteListener<TResult> listener) {
            return addOnCompleteListener(DirectExecutor.INSTANCE, listener);
        }

        @Override
        public Task<TResult> addOnCompleteListener(Executor executor,
                OnCompleteListener<TResult> listener) {
            registerCallback(() -> executor.execute(() -> listener.onComplete(this)));
            return this;
        }

        @Override
        public Task<TResult> addOnCanceledListener(OnCanceledListener listener) {
            return addOnCanceledListener(DirectExecutor.INSTANCE, listener);
        }

        @Override
        public Task<TResult> addOnCanceledListener(Executor executor,
                OnCanceledListener listener) {
            registerCallback(() -> {
                if (isCanceled()) {
                    executor.execute(listener::onCanceled);
                }
            });
            return this;
        }

        void setResult(TResult result) {
            if (!trySetResult(result)) {
                throw new IllegalStateException("Task is already complete");
            }
        }

        boolean trySetResult(TResult result) {
            List<Runnable> toRun;
            synchronized (mLock) {
                if (mComplete) return false;
                mComplete = true;
                mResult = result;
                toRun = new ArrayList<>(mCallbacks);
                mCallbacks.clear();
            }
            for (Runnable r : toRun) {
                r.run();
            }
            return true;
        }

        void setException(Exception e) {
            if (!trySetException(e)) {
                throw new IllegalStateException("Task is already complete");
            }
        }

        boolean trySetException(Exception e) {
            List<Runnable> toRun;
            synchronized (mLock) {
                if (mComplete) return false;
                mComplete = true;
                mException = e;
                toRun = new ArrayList<>(mCallbacks);
                mCallbacks.clear();
            }
            for (Runnable r : toRun) {
                r.run();
            }
            return true;
        }

        boolean cancel() {
            List<Runnable> toRun;
            synchronized (mLock) {
                if (mComplete) return false;
                mComplete = true;
                mCanceled = true;
                toRun = new ArrayList<>(mCallbacks);
                mCallbacks.clear();
            }
            for (Runnable r : toRun) {
                r.run();
            }
            return true;
        }

        private void registerCallback(Runnable runnable) {
            boolean runNow;
            synchronized (mLock) {
                runNow = mComplete;
                if (!runNow) {
                    mCallbacks.add(runnable);
                }
            }
            if (runNow) {
                runnable.run();
            }
        }
    }

    private static final class DirectExecutor implements Executor {
        static final DirectExecutor INSTANCE = new DirectExecutor();

        @Override
        public void execute(Runnable command) {
            command.run();
        }
    }
}
