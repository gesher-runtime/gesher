/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentSender;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.os.UserHandle;
import android.print.ILayoutResultCallback;
import android.print.IPrintDocumentAdapter;
import android.print.IPrintDocumentAdapterObserver;
import android.print.IPrintJobStateChangeListener;
import android.print.IPrintManager;
import android.print.IWriteResultCallback;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintJobId;
import android.print.PrintJobInfo;
import android.print.PrintManager;
import android.print.PrinterId;
import android.printservice.PrintServiceInfo;
import android.printservice.recommendation.RecommendationInfo;
import android.os.ICancellationSignal;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BooleanSupplier;

/**
 * The {@code print} service (IPrintManager; AOSP services/print PrintManagerService and
 * UserState at android-16.0.0_r4) over {@link PrintSpooler} and Windows printers.
 *
 * <p>{@code print()} creates a CREATED job and returns, as UserState does, the job and
 * a one-shot immutable activity IntentSender for {@link PrintManager#ACTION_PRINT_DIALOG}
 * with a {@code printjob:} URI, created for the printing app. PrintManager starts it
 * from the app's activity; {@link ActivityTaskManagerService} hands that start to
 * {@link #startDialog}, which pauses the app's activity, as PrintSpooler's PrintActivity
 * covers it, and runs the job: the adapter conversation, Windows' print dialog owned
 * by the app's window, and printing on the chosen Windows printer.
 *
 * <p>Windows printers are not Android PrintServices: getPrintServices and the print
 * service recommendations are empty, and printer discovery sessions (a system API the
 * spooler uses) are not offered. A job's printer id names this service's component
 * with the Windows printer name.
 */
final class PrintManagerService extends IPrintManager.Default {
    /** The component that printer ids of Windows printers name. */
    static final ComponentName WINDOWS_PRINTERS =
            new ComponentName("android", PrintManagerService.class.getName());

    private final ActivityManagerService mAm;
    private final PrintSpooler mSpooler;
    // Jobs whose dialog IntentSender was not started yet, by id; guarded by this.
    private final Map<String, Pending> mPending = new HashMap<>();
    // State listeners by appId, as UserState keeps them.
    private final Map<IBinder, Listener> mListeners = new HashMap<>();

    private static final class Pending {
        final String packageName;
        final PrintSpooler.Session session;

        Pending(String packageName, PrintSpooler.Session session) {
            this.packageName = packageName;
            this.session = session;
        }
    }

    private static final class Listener {
        final IPrintJobStateChangeListener listener;
        final int appId;

        Listener(IPrintJobStateChangeListener listener, int appId) {
            this.listener = listener;
            this.appId = appId;
        }
    }

    PrintManagerService(ActivityManagerService am, File directory, PrintSpooler.Host host) {
        mAm = am;
        directory.mkdirs();
        // Spooled documents of a previous run belong to jobs no app can see any more.
        File[] stale = directory.listFiles();
        if (stale != null) for (File file : stale) file.delete();
        mSpooler = new PrintSpooler(host, this::stateChanged, directory);
    }

    /** Whether the device offers the print framework (FEATURE_PRINTING): Windows printing is there. */
    static boolean supported() {
        return WindowsPrint.available();
    }

    /** UserState.resolveCallingAppEnforcingPermissions: an app acts only for its own appId. */
    private int appId(int claimed) {
        if (mAm.callers().isSystem()) return claimed;
        int own = UserHandle.getAppId(mAm.callers().callerUid());
        if (claimed != own && claimed != PrintManager.APP_ID_ANY) {
            throw new SecurityException("Call from app " + own + " as app " + claimed + " without permission");
        }
        return own;
    }

    private static void user(int userId) {
        if (userId != 0) throw new SecurityException("Only user 0 is available");
    }

    @Override
    public Bundle print(String printJobName, IPrintDocumentAdapter adapter, PrintAttributes attributes,
            String packageName, int appId, int userId) {
        java.util.Objects.requireNonNull(adapter);
        if (printJobName == null || printJobName.isEmpty()) {
            throw new IllegalArgumentException("printJobName cannot be empty");
        }
        if (packageName == null || packageName.isEmpty()) {
            throw new IllegalArgumentException("packageName cannot be empty");
        }
        user(userId);
        String caller = mAm.callers().requirePackage(packageName);
        int resolvedAppId = appId(appId);
        PrintJobId id = new PrintJobId();
        AdapterDocument document = new AdapterDocument(adapter);
        PrintSpooler.Session session = mSpooler.create(id.flattenToString(), resolvedAppId, printJobName,
                attributes, document, System.currentTimeMillis());
        document.session = session;
        // UserState: track the job so it is forgotten when its creator dies.
        IBinder binder = adapter.asBinder();
        try {
            binder.linkToDeath(new IBinder.DeathRecipient() {
                @Override public void binderDied() {
                    binder.unlinkToDeath(this, 0);
                    synchronized (PrintManagerService.this) { mPending.remove(session.id()); }
                    mSpooler.forgetApp(resolvedAppId);
                }
            }, 0);
        } catch (RemoteException dead) {
            mSpooler.forgetApp(resolvedAppId);
            return null;
        }
        Intent intent = new Intent(PrintManager.ACTION_PRINT_DIALOG);
        intent.setData(Uri.fromParts("printjob", id.flattenToString(), null));
        intent.putExtra(Intent.EXTRA_PACKAGE_NAME, caller);
        // Created under the app's own Binder identity, for the app.
        IIntentSender target = mAm.pendingIntents().get(ActivityManager.INTENT_SENDER_ACTIVITY, caller, 0,
                new Intent[] {intent}, new String[] {null},
                PendingIntent.FLAG_ONE_SHOT | PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        synchronized (this) { mPending.put(id.flattenToString(), new Pending(caller, session)); }
        Bundle result = new Bundle();
        result.putParcelable(PrintManager.EXTRA_PRINT_JOB, info(mSpooler.get(id.flattenToString(), resolvedAppId)));
        result.putParcelable(PrintManager.EXTRA_PRINT_DIALOG_INTENT, new IntentSender(target));
        return result;
    }

    /** Whether an activity start is the print dialog of a job (ATMS asks before resolving). */
    static boolean handles(Intent intent) {
        return intent != null && PrintManager.ACTION_PRINT_DIALOG.equals(intent.getAction())
                && intent.getComponent() == null && intent.getData() != null
                && "printjob".equals(intent.getData().getScheme());
    }

    /**
     * Starts the dialog of the job the intent names, for the app that created it.
     * {@code beforeDialog} runs on the job's thread before the dialog shows (the
     * covered activity's pause), {@code afterDialog} once the job left CREATED.
     * False when there is no such pending job of {@code creator}.
     */
    boolean startDialog(String creator, Intent intent, int ownerPid, Runnable beforeDialog,
            Runnable afterDialog) {
        String id = intent.getData().getSchemeSpecificPart();
        Pending pending;
        synchronized (this) {
            pending = mPending.get(id);
            if (pending == null || creator == null || !creator.equals(pending.packageName)) return false;
            mPending.remove(id);
        }
        PrintSpooler.Session session = pending.session;
        Thread worker = new Thread(() -> {
            try {
                if (!session.admit()) return;
                try {
                    beforeDialog.run();
                } finally {
                    try { session.run(ownerPid); }
                    finally { afterDialog.run(); }
                }
                mSpooler.printQueued(id);
            } catch (RuntimeException | Error failure) {
                SystemServer.log("print: job " + id + " failed: " + failure);
            }
        }, "print-" + id);
        worker.setDaemon(true);
        worker.start();
        return true;
    }

    @Override
    public List<PrintJobInfo> getPrintJobInfos(int appId, int userId) {
        user(userId);
        List<PrintJobInfo> result = new ArrayList<>();
        for (PrintSpooler.Snapshot job : mSpooler.jobs(appId(appId))) result.add(info(job));
        return result;
    }

    @Override
    public PrintJobInfo getPrintJobInfo(PrintJobId printJobId, int appId, int userId) {
        user(userId);
        if (printJobId == null) return null;
        PrintSpooler.Snapshot job = mSpooler.get(printJobId.flattenToString(), appId(appId));
        return job == null ? null : info(job);
    }

    @Override
    public void cancelPrintJob(PrintJobId printJobId, int appId, int userId) {
        user(userId);
        if (printJobId == null) return;
        mSpooler.cancel(printJobId.flattenToString(), appId(appId));
    }

    @Override
    public void restartPrintJob(PrintJobId printJobId, int appId, int userId) {
        user(userId);
        if (printJobId == null) return;
        String id = printJobId.flattenToString();
        if (!mSpooler.restart(id, appId(appId))) return;
        Thread worker = new Thread(() -> mSpooler.printQueued(id), "print-" + id);
        worker.setDaemon(true);
        worker.start();
    }

    @Override
    public void addPrintJobStateChangeListener(IPrintJobStateChangeListener listener, int appId, int userId) {
        user(userId);
        int resolved = appId(appId);
        IBinder binder = listener.asBinder();
        try {
            binder.linkToDeath(() -> {
                synchronized (PrintManagerService.this) { mListeners.remove(binder); }
            }, 0);
        } catch (RemoteException dead) {
            return;
        }
        synchronized (this) { mListeners.put(binder, new Listener(listener, resolved)); }
    }

    @Override
    public void removePrintJobStateChangeListener(IPrintJobStateChangeListener listener, int userId) {
        user(userId);
        synchronized (this) { mListeners.remove(listener.asBinder()); }
    }

    @Override
    public List<PrintServiceInfo> getPrintServices(int selectionFlags, int userId) {
        user(userId);
        return new ArrayList<>();
    }

    @Override
    public List<RecommendationInfo> getPrintServiceRecommendations(int userId) {
        user(userId);
        return new ArrayList<>();
    }

    @Override
    public boolean isPrintServiceEnabled(ComponentName service, int userId) {
        user(userId);
        return false;
    }

    private void stateChanged(PrintSpooler.Snapshot job) {
        List<IPrintJobStateChangeListener> targets = new ArrayList<>();
        synchronized (this) {
            for (Listener listener : mListeners.values()) {
                if (listener.appId == job.appId || listener.appId == PrintManager.APP_ID_ANY) {
                    targets.add(listener.listener);
                }
            }
        }
        PrintJobId id = PrintJobId.unflattenFromString(job.id);
        for (IPrintJobStateChangeListener listener : targets) {
            try {
                listener.onPrintJobStateChanged(id);
            } catch (RemoteException | RuntimeException gone) {
                // A dead listener is removed by its death recipient.
            }
        }
    }

    static PrintJobInfo info(PrintSpooler.Snapshot job) {
        PrintJobInfo info = new PrintJobInfo();
        info.setId(PrintJobId.unflattenFromString(job.id));
        info.setAppId(job.appId);
        info.setLabel(job.label);
        info.setCreationTime(job.creationTime);
        info.setState(job.state);
        info.setCopies(job.copies);
        if (job.status != null) info.setStatus(job.status);
        if (job.attributes instanceof PrintAttributes attributes) info.setAttributes(attributes);
        if (job.documentInfo instanceof PrintDocumentInfo document) info.setDocumentInfo(document);
        if (job.pages != null) info.setPages(pageRanges(job.pages));
        if (job.printer != null) {
            info.setPrinterName(job.printer);
            info.setPrinterId(new PrinterId(WINDOWS_PRINTERS, job.printer));
        }
        return info;
    }

    static PageRange[] pageRanges(int[] pages) {
        if (PrintSpooler.isAll(pages)) return new PageRange[] {PageRange.ALL_PAGES};
        PageRange[] ranges = new PageRange[pages.length / 2];
        for (int i = 0; i < ranges.length; i++) ranges[i] = new PageRange(pages[2 * i], pages[2 * i + 1]);
        return ranges;
    }

    static int[] pages(PageRange[] ranges) {
        if (ranges == null) return null;
        int[] pages = new int[ranges.length * 2];
        for (int i = 0; i < ranges.length; i++) {
            if (ranges[i] == null) return null;
            pages[2 * i] = ranges[i].getStart();
            pages[2 * i + 1] = ranges[i].getEnd();
        }
        return pages;
    }

    /** The app's IPrintDocumentAdapter, with the callbacks RemotePrintDocument gives it. */
    private static final class AdapterDocument implements PrintSpooler.Document {
        private final IPrintDocumentAdapter mAdapter;
        volatile PrintSpooler.Session session;

        AdapterDocument(IPrintDocumentAdapter adapter) {
            mAdapter = adapter;
        }

        @Override
        public void start() throws RemoteException {
            // PrintDocumentAdapterDelegate reports its activity's destruction here.
            mAdapter.setObserver(new IPrintDocumentAdapterObserver.Stub() {
                @Override public void onDestroy() { session.destroyed(); }
            });
            mAdapter.start();
        }

        @Override
        public void layout(Object oldAttributes, Object newAttributes, boolean preview, int sequence)
                throws RemoteException {
            Bundle metadata = new Bundle();
            metadata.putBoolean(PrintDocumentAdapter.EXTRA_PRINT_PREVIEW, preview);
            // RemotePrintDocument passes empty attributes before the first layout.
            PrintAttributes old = oldAttributes instanceof PrintAttributes attributes ? attributes
                    : new PrintAttributes.Builder().build();
            mAdapter.layout(old, (PrintAttributes) newAttributes, new ILayoutResultCallback.Stub() {
                @Override public void onLayoutStarted(ICancellationSignal cancellation, int seq) {
                    session.started(true, seq, canceler(cancellation));
                }
                @Override public void onLayoutFinished(PrintDocumentInfo info, boolean changed, int seq) {
                    session.layoutFinished(seq, info,
                            info == null ? PrintSpooler.PAGE_COUNT_UNKNOWN : info.getPageCount(), changed);
                }
                @Override public void onLayoutFailed(CharSequence error, int seq) {
                    session.failed(true, seq, error);
                }
                @Override public void onLayoutCanceled(int seq) {
                    session.canceled(true, seq);
                }
            }, metadata, sequence);
        }

        @Override
        public void write(int[] pages, File destination, int sequence) throws Exception {
            ParcelFileDescriptor fd = ParcelFileDescriptor.open(destination, ParcelFileDescriptor.MODE_WRITE_ONLY
                    | ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE);
            try {
                mAdapter.write(pageRanges(pages), fd, new IWriteResultCallback.Stub() {
                    @Override public void onWriteStarted(ICancellationSignal cancellation, int seq) {
                        session.started(false, seq, canceler(cancellation));
                    }
                    @Override public void onWriteFinished(PageRange[] written, int seq) {
                        session.writeFinished(seq, PrintSpooler.normalize(PrintManagerService.pages(written)));
                    }
                    @Override public void onWriteFailed(CharSequence error, int seq) {
                        session.failed(false, seq, error);
                    }
                    @Override public void onWriteCanceled(int seq) {
                        session.canceled(false, seq);
                    }
                }, sequence);
            } finally {
                // Across processes the transaction gave the app its own descriptor. In the
                // app's own process the delegate keeps this one and closes it when done.
                if (mAdapter.asBinder().queryLocalInterface("android.print.IPrintDocumentAdapter") == null) {
                    fd.close();
                }
            }
        }

        @Override
        public void finish() throws RemoteException {
            mAdapter.finish();
        }

        private static Runnable canceler(ICancellationSignal cancellation) {
            return cancellation == null ? null : () -> {
                try {
                    cancellation.cancel();
                } catch (RemoteException | RuntimeException gone) {
                    // The app is gone; its death cancels the job.
                }
            };
        }
    }
}
