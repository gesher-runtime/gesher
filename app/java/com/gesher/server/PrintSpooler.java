/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.File;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BooleanSupplier;

/**
 * Print jobs and the PrintDocumentAdapter lifecycle that AOSP's PrintSpooler drives
 * (frameworks/base packages/PrintSpooler, android-16.0.0_r4: PrintActivity and
 * model/RemotePrintDocument), with the printer chosen in Windows' print dialog and
 * the document printed by Windows instead of a PrintService.
 *
 * <p>This class is plain Java so its state machine runs unchanged in a host harness
 * (tools/test_print_spooler_contracts.py). {@link PrintManagerService} adapts it to
 * {@code IPrintManager}, {@code IPrintDocumentAdapter} and {@code PrintJobInfo};
 * attributes and document information stay opaque here.
 *
 * <p>A job follows the spooler's order: {@code start()}, a preview layout with the
 * default printer's attributes, the user's choice, a final layout with the chosen
 * attributes ({@code EXTRA_PRINT_PREVIEW} false, which RemotePrintDocument treats as a
 * new constraint), {@code write()} of the chosen pages, {@code finish()}, then
 * QUEUED. Printing moves it to STARTED and to COMPLETED only after Windows accepted
 * the whole document; a Windows failure is FAILED (restartable), and a dismissed
 * dialog, an app that went away or an adapter failure is CANCELED, as
 * PrintActivity.onPause and onPrintDocumentError leave it.
 */
final class PrintSpooler {
    // PrintJobInfo.STATE_* (PrintJobInfoProto values).
    static final int STATE_CREATED = 1;
    static final int STATE_QUEUED = 2;
    static final int STATE_STARTED = 3;
    static final int STATE_BLOCKED = 4;
    static final int STATE_COMPLETED = 5;
    static final int STATE_FAILED = 6;
    static final int STATE_CANCELED = 7;
    /** PrintDocumentInfo.PAGE_COUNT_UNKNOWN. */
    static final int PAGE_COUNT_UNKNOWN = -1;
    /** PageRange.ALL_PAGES as a flat [start, end] pair. */
    static final int[] ALL_PAGES = {0, Integer.MAX_VALUE};
    /** Concurrent dialogs; another print() while this many are open is canceled. */
    static final int MAX_SESSIONS = 8;

    /** The app's adapter, reached through the oneway IPrintDocumentAdapter. */
    interface Document {
        void start() throws Exception;
        /** {@code oldAttributes} null: none yet (the spooler passes empty attributes). */
        void layout(Object oldAttributes, Object newAttributes, boolean preview, int sequence)
                throws Exception;
        void write(int[] pages, File destination, int sequence) throws Exception;
        void finish() throws Exception;
    }

    /** A printer and the attributes, pages and copies to print with. */
    static final class Choice {
        final String printer;
        final Object attributes;
        final int[] pages;
        final int copies;
        /** The Windows settings (a DEVMODE) the attributes describe. */
        final Object settings;

        Choice(String printer, Object attributes, int[] pages, int copies, Object settings) {
            if (printer == null || printer.isEmpty() || attributes == null) {
                throw new IllegalArgumentException("A choice names a printer and its attributes");
            }
            this.printer = printer;
            this.attributes = attributes;
            this.pages = pages == null ? ALL_PAGES.clone() : pages.clone();
            this.copies = Math.max(1, copies);
            this.settings = settings;
        }
    }

    /** Windows: the default printer, the user's dialog and the print itself. */
    interface Host {
        /** The default printer with the app's requested attributes applied; null: no printer. */
        Choice defaults(Object requestedAttributes) throws IOException;
        /** The print dialog; null when the user canceled it or {@code active} became false. */
        Choice choose(String label, int owner, Choice initial, int pageCount, BooleanSupplier active)
                throws IOException;
        /**
         * Prints {@code filePages} of {@code pdf}. Returns only when Windows accepted the
         * whole job; {@link InterruptedIOException} when {@code active} became false.
         */
        void print(String label, Choice choice, File pdf, int[] filePages, BooleanSupplier active)
                throws IOException;
    }

    interface Listener {
        void stateChanged(Snapshot job);
    }

    /** What getPrintJobInfo reports. */
    static final class Snapshot {
        final String id;
        final int appId;
        final String label;
        final long creationTime;
        final int state;
        final String status;
        final String printer;
        final Object attributes;
        final int copies;
        final int[] pages;
        final Object documentInfo;

        Snapshot(Job job) {
            id = job.id;
            appId = job.appId;
            label = job.label;
            creationTime = job.creationTime;
            state = job.state;
            status = job.status;
            printer = job.choice == null ? null : job.choice.printer;
            attributes = job.choice == null ? job.requested : job.choice.attributes;
            copies = job.choice == null ? 1 : job.choice.copies;
            pages = job.pages == null ? null : job.pages.clone();
            documentInfo = job.documentInfo;
        }
    }

    final class Job {
        final String id;
        final int appId;
        final String label;
        final Object requested;
        final long creationTime;
        // Guarded by PrintSpooler.this.
        int state = STATE_CREATED;
        String status;
        Choice choice;
        int[] pages;
        Object documentInfo;
        File file;
        int[] filePages;
        boolean printing;
        boolean cancelRequested;
        Session session;

        Job(String id, int appId, String label, Object requested, long creationTime) {
            this.id = id;
            this.appId = appId;
            this.label = label;
            this.requested = requested;
            this.creationTime = creationTime;
        }
    }

    private final Host mHost;
    private final Listener mListener;
    private final File mDirectory;
    private final long mCallbackTimeoutMillis;
    private final long mCancelTimeoutMillis;
    // Insertion ordered, as the spooler lists them; guarded by this.
    private final Map<String, Job> mJobs = new LinkedHashMap<>();
    private int mSessions;

    PrintSpooler(Host host, Listener listener, File directory) {
        this(host, listener, directory, 120_000, 10_000);
    }

    PrintSpooler(Host host, Listener listener, File directory, long callbackTimeoutMillis,
            long cancelTimeoutMillis) {
        mHost = host;
        mListener = listener;
        mDirectory = directory;
        mCallbackTimeoutMillis = callbackTimeoutMillis;
        mCancelTimeoutMillis = cancelTimeoutMillis;
    }

    /**
     * A new CREATED job (UserState.print). The returned session is started by the
     * caller with {@link Session#run} on a thread of its own once the dialog may show.
     */
    synchronized Session create(String id, int appId, String label, Object requested,
            Document document, long now) {
        if (id == null || label == null || label.isEmpty() || document == null) {
            throw new IllegalArgumentException("A print job needs an id, a label and a document");
        }
        if (mJobs.containsKey(id)) throw new IllegalArgumentException("Duplicate print job " + id);
        Job job = new Job(id, appId, label, requested, now);
        mJobs.put(id, job);
        Session session = new Session(job, document);
        job.session = session;
        return session;
    }

    synchronized Snapshot get(String id, int appId) {
        Job job = mJobs.get(id);
        return job == null || job.appId != appId ? null : new Snapshot(job);
    }

    synchronized List<Snapshot> jobs(int appId) {
        List<Snapshot> result = new ArrayList<>();
        for (Job job : mJobs.values()) if (job.appId == appId) result.add(new Snapshot(job));
        return result;
    }

    /** PrintJobForAppCache: the app's creator binder died, so its jobs are forgotten. */
    void forgetApp(int appId) {
        List<Session> sessions = new ArrayList<>();
        synchronized (this) {
            for (Job job : mJobs.values()) {
                if (job.appId == appId && job.session != null) sessions.add(job.session);
            }
        }
        for (Session session : sessions) session.destroyed();
        synchronized (this) {
            mJobs.values().removeIf(job -> {
                if (job.appId != appId || job.printing) return false;
                deleteFile(job);
                return true;
            });
        }
    }

    /**
     * UserState.cancelPrintJob: a job without a printer (still in its dialog) is left
     * to the user, as in AOSP. A queued or failed job is canceled; a printing job
     * is canceled when Windows printing stops.
     */
    void cancel(String id, int appId) {
        Snapshot changed = null;
        synchronized (this) {
            Job job = mJobs.get(id);
            if (job == null || job.appId != appId) return;
            switch (job.state) {
                case STATE_QUEUED:
                case STATE_FAILED:
                    if (job.printing) {
                        job.cancelRequested = true;
                    } else {
                        changed = setStateLocked(job, STATE_CANCELED, null);
                        deleteFile(job);
                    }
                    break;
                case STATE_STARTED:
                case STATE_BLOCKED:
                    job.cancelRequested = true;
                    notifyAll();
                    break;
                default:
                    break;
            }
        }
        dispatch(changed);
    }

    /** UserState.restartPrintJob: only a FAILED job is queued again. */
    boolean restart(String id, int appId) {
        Snapshot changed;
        synchronized (this) {
            Job job = mJobs.get(id);
            if (job == null || job.appId != appId || job.state != STATE_FAILED || job.printing
                    || job.file == null) return false;
            job.cancelRequested = false;
            job.printing = true;
            changed = setStateLocked(job, STATE_QUEUED, null);
        }
        dispatch(changed);
        return true;
    }

    /** Prints a QUEUED job; runs on the caller's thread. */
    void printQueued(String id) {
        Job job;
        Snapshot changed;
        synchronized (this) {
            job = mJobs.get(id);
            if (job == null || job.state != STATE_QUEUED || !job.printing) return;
            if (job.cancelRequested) {
                job.printing = false;
                changed = setStateLocked(job, STATE_CANCELED, null);
                deleteFile(job);
                job = null;
            } else {
                changed = setStateLocked(job, STATE_STARTED, null);
            }
        }
        dispatch(changed);
        if (job == null) return;
        final Job printing = job;
        BooleanSupplier active = () -> {
            synchronized (PrintSpooler.this) { return !printing.cancelRequested; }
        };
        int state;
        String status = null;
        try {
            mHost.print(printing.label, printing.choice, printing.file, printing.filePages, active);
            // Windows accepted every page; a cancel that came later does not withdraw them.
            state = STATE_COMPLETED;
        } catch (InterruptedIOException canceled) {
            state = STATE_CANCELED;
        } catch (IOException | RuntimeException failure) {
            synchronized (this) {
                state = printing.cancelRequested ? STATE_CANCELED : STATE_FAILED;
            }
            status = failure.getMessage();
        }
        synchronized (this) {
            printing.printing = false;
            changed = setStateLocked(printing, state, state == STATE_FAILED ? status : null);
            if (state != STATE_FAILED) deleteFile(printing);
        }
        dispatch(changed);
    }

    private Snapshot setStateLocked(Job job, int state, String status) {
        if (job.state == state && java.util.Objects.equals(job.status, status)) return null;
        job.state = state;
        job.status = status;
        notifyAll();
        return new Snapshot(job);
    }

    private void dispatch(Snapshot changed) {
        if (changed != null && mListener != null) mListener.stateChanged(changed);
    }

    private static void deleteFile(Job job) {
        if (job.file != null) {
            job.file.delete();
            job.file = null;
        }
    }

    /** The adapter conversation for one job, from start() to finish(). */
    final class Session {
        final Job job;
        private final Document mDocument;
        // Guarded by PrintSpooler.this.
        private boolean mDestroyed;
        private int mSequence;
        private Pending mPending;
        private boolean mCounted;

        Session(Job job, Document document) {
            this.job = job;
            mDocument = document;
        }

        String id() {
            return job.id;
        }

        /** The adapter's observer onDestroy, or its process died: the dialog is abandoned. */
        void destroyed() {
            Runnable cancel = null;
            synchronized (PrintSpooler.this) {
                if (mDestroyed) return;
                mDestroyed = true;
                if (mPending != null && !mPending.done) cancel = mPending.takeCancel();
                PrintSpooler.this.notifyAll();
            }
            if (cancel != null) cancel.run();
        }

        boolean active() {
            synchronized (PrintSpooler.this) {
                return !mDestroyed && job.state == STATE_CREATED;
            }
        }

        /** Whether this session may run now; false cancels the job (too many dialogs). */
        boolean admit() {
            Snapshot changed = null;
            synchronized (PrintSpooler.this) {
                if (mSessions < MAX_SESSIONS) {
                    mSessions++;
                    mCounted = true;
                    return true;
                }
                changed = setStateLocked(job, STATE_CANCELED, null);
            }
            dispatch(changed);
            callQuietly(mDocument::start);
            callQuietly(mDocument::finish);
            return false;
        }

        /**
         * The adapter conversation, its dialog owned by the window of process
         * {@code owner} (0: none); returns once the job left CREATED. A QUEUED job is
         * then printed by {@link #printQueued}.
         */
        void run(int owner) {
            boolean started = false;
            String status = null;
            int outcome = STATE_CANCELED;
            try {
                if (!active()) return;
                mDocument.start();
                started = true;
                Choice defaults = mHost.defaults(job.requested);
                Object current = null;
                int pageCount = PAGE_COUNT_UNKNOWN;
                if (defaults != null) {
                    Pending layout = command(true, sequence -> mDocument.layout(null,
                            defaults.attributes, true, sequence));
                    if (layout.outcome != Pending.FINISHED) {
                        status = layout.error;
                        return;
                    }
                    current = defaults.attributes;
                    pageCount = layout.pageCount;
                    synchronized (PrintSpooler.this) { job.documentInfo = layout.info; }
                }
                if (!active()) return;
                Choice choice = mHost.choose(job.label, owner, defaults, pageCount, this::active);
                if (choice == null || !active()) return;
                synchronized (PrintSpooler.this) { job.choice = choice; }
                final Object previous = current;
                Pending layout = command(true, sequence -> mDocument.layout(previous,
                        choice.attributes, false, sequence));
                if (layout.outcome != Pending.FINISHED) {
                    status = layout.error;
                    return;
                }
                pageCount = layout.pageCount;
                int[] requested = selectedPages(choice.pages, pageCount);
                synchronized (PrintSpooler.this) {
                    job.documentInfo = layout.info;
                    job.pages = requested.clone();
                }
                File file = new File(mDirectory, job.id + ".pdf");
                file.delete();
                synchronized (PrintSpooler.this) { job.file = file; }
                Pending write = command(false,
                        sequence -> mDocument.write(requested.clone(), file, sequence));
                if (write.outcome != Pending.FINISHED) {
                    status = write.error;
                    return;
                }
                int[] filePages = filePages(requested, normalize(write.pages), pageCount);
                if (filePages == null) {
                    status = "The app wrote pages other than the requested ones";
                    return;
                }
                synchronized (PrintSpooler.this) {
                    if (!active()) return;
                    job.filePages = filePages;
                    job.printing = true;
                    outcome = STATE_QUEUED;
                }
            } catch (InterruptedIOException canceled) {
                status = null;
            } catch (Exception failure) {
                status = failure.toString();
            } finally {
                // PrintActivity finishes the document when it leaves, whatever the outcome.
                if (started) callQuietly(mDocument::finish);
                Snapshot changed;
                synchronized (PrintSpooler.this) {
                    if (mCounted) { mSessions--; mCounted = false; }
                    job.session = null;
                    if (outcome != STATE_QUEUED) {
                        job.printing = false;
                        deleteFile(job);
                    }
                    changed = job.state == STATE_CREATED
                            ? setStateLocked(job, outcome, outcome == STATE_CANCELED ? status : null)
                            : null;
                }
                dispatch(changed);
            }
        }

        private Pending command(boolean layout, Command call) throws Exception {
            Pending pending;
            synchronized (PrintSpooler.this) {
                if (!active()) throw new InterruptedIOException("Print dialog closed");
                pending = new Pending(layout, ++mSequence);
                mPending = pending;
            }
            call.run(pending.sequence);
            long deadline = System.nanoTime() + mCallbackTimeoutMillis * 1_000_000L;
            Runnable cancel = null;
            synchronized (PrintSpooler.this) {
                while (!pending.done && !mDestroyed) {
                    long left = (deadline - System.nanoTime()) / 1_000_000L;
                    if (left <= 0) break;
                    PrintSpooler.this.wait(left);
                }
                if (pending.done) {
                    mPending = null;
                    return pending;
                }
                cancel = pending.takeCancel();
                pending.error = mDestroyed ? null
                        : (layout ? "Layout" : "Write") + " did not finish";
            }
            // RemotePrintDocument cancels a running command through the app's
            // ICancellationSignal and waits for its canceled (or late) result.
            if (cancel != null) cancel.run();
            deadline = System.nanoTime() + mCancelTimeoutMillis * 1_000_000L;
            synchronized (PrintSpooler.this) {
                while (!pending.done) {
                    long left = (deadline - System.nanoTime()) / 1_000_000L;
                    if (left <= 0) break;
                    PrintSpooler.this.wait(left);
                }
                mPending = null;
                pending.done = true;
                if (pending.outcome == Pending.FINISHED && mDestroyed) pending.outcome = Pending.CANCELED;
                if (pending.outcome == Pending.FINISHED && pending.error != null) {
                    pending.outcome = Pending.FAILED; // a late result after a time-out
                } else if (pending.outcome == Pending.NONE) {
                    pending.outcome = Pending.CANCELED;
                }
            }
            if (pending.outcome == Pending.CANCELED && mDestroyedSnapshot()) {
                throw new InterruptedIOException("Print dialog closed");
            }
            return pending;
        }

        private boolean mDestroyedSnapshot() {
            synchronized (PrintSpooler.this) { return mDestroyed; }
        }

        // The callbacks of ILayoutResultCallback and IWriteResultCallback; a stale
        // sequence or a callback of the other kind is ignored, as RemotePrintDocument does.

        void started(boolean layout, int sequence, Runnable cancel) {
            boolean cancelNow;
            synchronized (PrintSpooler.this) {
                Pending pending = current(layout, sequence);
                if (pending == null) return;
                pending.cancel = cancel;
                cancelNow = mDestroyed && pending.takeCancel() != null;
            }
            if (cancelNow) cancel.run();
        }

        void layoutFinished(int sequence, Object info, int pageCount, boolean changed) {
            synchronized (PrintSpooler.this) {
                Pending pending = current(true, sequence);
                if (pending == null) return;
                pending.info = info;
                pending.pageCount = pageCount;
                complete(pending, Pending.FINISHED, null);
            }
        }

        void writeFinished(int sequence, int[] pages) {
            synchronized (PrintSpooler.this) {
                Pending pending = current(false, sequence);
                if (pending == null) return;
                pending.pages = pages == null ? null : pages.clone();
                complete(pending, Pending.FINISHED, null);
            }
        }

        void failed(boolean layout, int sequence, CharSequence error) {
            synchronized (PrintSpooler.this) {
                Pending pending = current(layout, sequence);
                if (pending == null) return;
                complete(pending, Pending.FAILED, error == null ? null : error.toString());
            }
        }

        void canceled(boolean layout, int sequence) {
            synchronized (PrintSpooler.this) {
                Pending pending = current(layout, sequence);
                if (pending == null) return;
                complete(pending, Pending.CANCELED, pending.error);
            }
        }

        private Pending current(boolean layout, int sequence) {
            Pending pending = mPending;
            return pending == null || pending.done || pending.layout != layout
                    || pending.sequence != sequence ? null : pending;
        }

        private void complete(Pending pending, int outcome, String error) {
            pending.outcome = outcome;
            if (error != null) pending.error = error;
            pending.done = true;
            PrintSpooler.this.notifyAll();
        }
    }

    private static final class Pending {
        static final int NONE = 0, FINISHED = 1, FAILED = 2, CANCELED = 3;
        final boolean layout;
        final int sequence;
        Runnable cancel;
        boolean cancelSent;
        boolean done;
        int outcome = NONE;
        String error;
        Object info;
        int pageCount = PAGE_COUNT_UNKNOWN;
        int[] pages;

        Pending(boolean layout, int sequence) {
            this.layout = layout;
            this.sequence = sequence;
        }

        /** The cancellation to send, once; null before onLayoutStarted/onWriteStarted. */
        Runnable takeCancel() {
            if (cancel == null || cancelSent) return null;
            cancelSent = true;
            return cancel;
        }
    }

    private interface Command {
        void run(int sequence) throws Exception;
    }

    private interface ThrowingRunnable {
        void run() throws Exception;
    }

    private static void callQuietly(ThrowingRunnable call) {
        try {
            call.run();
        } catch (Exception ignored) {
            // A dead adapter has nothing left to finish.
        }
    }

    // Page ranges as flat [start0, end0, start1, end1, ...] arrays, zero based and
    // inclusive, as PageRange and the spooler's PageRangeUtils use them.

    static boolean isAll(int[] ranges) {
        return ranges != null && Arrays.equals(ranges, ALL_PAGES);
    }

    /** PageRangeUtils.normalize: sorted, merged and non-overlapping; null for invalid ranges. */
    static int[] normalize(int[] ranges) {
        if (ranges == null || ranges.length == 0 || ranges.length % 2 != 0) return null;
        List<int[]> list = new ArrayList<>();
        for (int i = 0; i < ranges.length; i += 2) {
            if (ranges[i] < 0 || ranges[i] > ranges[i + 1]) return null;
            list.add(new int[] {ranges[i], ranges[i + 1]});
        }
        list.sort(Comparator.comparingInt((int[] range) -> range[0]));
        List<int[]> merged = new ArrayList<>();
        for (int[] range : list) {
            int[] last = merged.isEmpty() ? null : merged.get(merged.size() - 1);
            if (last != null && (long) last[1] + 1 >= range[0]) {
                last[1] = Math.max(last[1], range[1]);
            } else {
                merged.add(range);
            }
        }
        int[] result = new int[merged.size() * 2];
        for (int i = 0; i < merged.size(); i++) {
            result[2 * i] = merged.get(i)[0];
            result[2 * i + 1] = merged.get(i)[1];
        }
        return result;
    }

    /**
     * The pages to request from the app (PrintActivity.updateSelectedPages and its
     * clipping to the laid-out count): ALL_PAGES for every page, ranges beyond the
     * document dropped or cut at its end.
     */
    static int[] selectedPages(int[] chosen, int pageCount) {
        int[] pages = normalize(chosen);
        if (pages == null || isAll(pages)) return ALL_PAGES.clone();
        if (pageCount <= 0) return pages;
        List<Integer> kept = new ArrayList<>();
        for (int i = 0; i < pages.length; i += 2) {
            if (pages[i] >= pageCount) break;
            kept.add(pages[i]);
            kept.add(Math.min(pages[i + 1], pageCount - 1));
        }
        if (kept.isEmpty()) return ALL_PAGES.clone();
        int[] result = new int[kept.size()];
        for (int i = 0; i < result.length; i++) result[i] = kept.get(i);
        if (result.length == 2 && result[0] == 0 && result[1] == pageCount - 1) return ALL_PAGES.clone();
        return result;
    }

    /** PageRangeUtils.contains(ours, others, pageCount). */
    static boolean contains(int[] ours, int[] others, int pageCount) {
        if (ours == null || others == null) return false;
        if (isAll(ours)) return true;
        if (isAll(others)) {
            if (pageCount <= 0) return false;
            others = new int[] {0, pageCount - 1};
        }
        ours = normalize(ours);
        others = normalize(others);
        if (ours == null || others == null) return false;
        int o = 0;
        for (int i = 0; i < ours.length; i += 2) {
            for (; o < others.length; o += 2) {
                if (others[o] > ours[i + 1]) break;
                if (others[o] < ours[i] || others[o + 1] > ours[i + 1]) return false;
            }
        }
        return o >= others.length;
    }

    /**
     * PageRangeUtils.computeWhichPagesInFileToPrint: the written file's own page
     * indices to print, or null when the app did not write the requested pages.
     * Unlike AOSP's version, the "more pages than requested" case returns the
     * requested pages offset into the file (AOSP offsets a discarded clone).
     */
    static int[] filePages(int[] requested, int[] written, int pageCount) {
        if (written == null || requested == null) return null;
        if (isAll(requested) && pageCount == PAGE_COUNT_UNKNOWN) return ALL_PAGES.clone();
        if (Arrays.equals(written, requested)) return ALL_PAGES.clone();
        if (isAll(written)) return requested.clone();
        if (isAll(requested) && pageCount > 0 && written.length == 2 && written[0] == 0
                && written[1] == pageCount - 1) {
            return ALL_PAGES.clone();
        }
        if (contains(written, requested, pageCount)) {
            int[] wanted = isAll(requested) ? new int[] {0, pageCount - 1} : requested;
            // Each requested page's position among the written pages.
            List<Integer> result = new ArrayList<>();
            for (int i = 0; i < wanted.length; i += 2) {
                for (int page = wanted[i]; page <= wanted[i + 1]; page++) {
                    int index = indexIn(written, page);
                    if (index < 0) return null;
                    int size = result.size();
                    if (size > 0 && result.get(size - 1) == index - 1) {
                        result.set(size - 1, index);
                    } else {
                        result.add(index);
                        result.add(index);
                    }
                }
            }
            int[] array = new int[result.size()];
            for (int i = 0; i < array.length; i++) array[i] = result.get(i);
            return array;
        }
        return null;
    }

    private static int indexIn(int[] written, int page) {
        int index = 0;
        for (int i = 0; i < written.length; i += 2) {
            if (page >= written[i] && page <= written[i + 1]) return index + page - written[i];
            index += written[i + 1] - written[i] + 1;
        }
        return -1;
    }
}
