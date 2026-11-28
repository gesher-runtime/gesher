/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.app.AppOpsManager;
import android.util.AtomicFile;
import com.android.internal.app.IAppOpsService;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.management.ManagementFactory;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Complete TestHarness teardown, production writer/serializer and real files.
 * Only a finishWrite barrier controls when the actual atomic rename completes.
 */
public final class ExactAlarmPersistenceHost {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field value=type.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Object get(Object owner, String name) throws Exception {
        return field(owner.getClass(),name).get(owner);
    }
    private static Object call(Object owner, String name) throws Exception {
        Method value=owner.getClass().getDeclaredMethod(name); value.setAccessible(true);
        try { return value.invoke(owner); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception cause) throw cause;
            if (failure.getCause() instanceof Error cause) throw cause;
            throw failure;
        }
    }
    private static final class Harness implements AutoCloseable {
        final Object fixture, store, writer;
        final AtomicFile file;
        final IAppOpsService ops;
        Harness() throws Exception {
            Class<?> type=Class.forName("gesher.services.ExactAlarmContractTest$TestHarness");
            Constructor<?> constructor=type.getDeclaredConstructor(boolean.class);
            constructor.setAccessible(true); fixture=constructor.newInstance(false);
            Object implementation=call(get(fixture,"pm"),"appOps");
            store=get(implementation,"mStore"); writer=get(store,"mWriter");
            file=(AtomicFile)get(store,"mFile"); ops=(IAppOpsService)get(fixture,"appOps");
        }
        void allow() throws Exception {
            Class<?> type=Class.forName("gesher.services.ExactAlarmContractTest");
            ops.setMode(AppOpsManager.OP_SCHEDULE_EXACT_ALARM,
                    (int)field(type,"EXACT_UID").get(null),
                    (String)field(type,"EXACT_PKG").get(null),AppOpsManager.MODE_ALLOWED);
        }
        boolean flush() throws Exception { return (Boolean)call(store,"flush"); }
        boolean clean() throws Exception {
            synchronized(get(writer,"mScheduleLock")) {
                return (long)get(writer,"mVersion")== (long)get(writer,"mWrittenVersion")
                        && get(writer,"mTask")==null;
            }
        }
        @Override public void close() throws Exception { call(fixture,"close"); }
    }
    private static final class HeldAtomicFile extends AtomicFile {
        final CountDownLatch entered=new CountDownLatch(1), release=new CountDownLatch(1);
        final AtomicBoolean first=new AtomicBoolean(true);
        HeldAtomicFile(File file) { super(file); }
        @Override public void finishWrite(FileOutputStream stream) {
            if (first.compareAndSet(true,false)) {
                entered.countDown();
                try {
                    if (!release.await(5,TimeUnit.SECONDS)) throw new AssertionError("Held atomic writer was not released");
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt(); throw new AssertionError(failure);
                }
            }
            super.finishWrite(stream);
        }
    }
    private static void join(Thread thread) throws Exception {
        thread.join(5000); check(!thread.isAlive(),"Actual fixture worker did not retire: "+thread.getName());
    }
    private static void pendingWrite() throws Exception {
        try (Harness h=new Harness()) {
            ScheduledThreadPoolExecutor io=(ScheduledThreadPoolExecutor)field(h.writer.getClass(),"IO").get(null);
            CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);
            Future<?> blocker=io.submit(()->{
                entered.countDown();
                try { if (!release.await(5,TimeUnit.SECONDS)) throw new AssertionError("Policy IO blocker was not released"); }
                catch (InterruptedException failure) { Thread.currentThread().interrupt(); throw new AssertionError(failure); }
            });
            try {
                check(entered.await(5,TimeUnit.SECONDS),"The real shared policy queue was not held");
                h.allow();
                check(!h.clean(),"Real AppOps mutation did not schedule persistence");
                h.close();
                check(h.clean(),"Fixture returned with its final AppOps write still pending");
                check(h.file.getBaseFile().isFile(),"Fixture final state was not written to the real file");
                check(!Files.exists(Path.of(h.file.getBaseFile()+".new")),"Pending write left an atomic temporary file");
            } finally {
                release.countDown(); blocker.get(5,TimeUnit.SECONDS);
                h.flush(); // Also retire the original fixture's failed negative control.
            }
        }
    }
    private static void inFlightWrite() throws Exception {
        try (Harness h=new Harness()) {
            HeldAtomicFile held=new HeldAtomicFile(h.file.getBaseFile());
            field(h.store.getClass(),"mFile").set(h.store,held);
            AtomicReference<Throwable> writerFailure=new AtomicReference<>(),closeFailure=new AtomicReference<>();
            CountDownLatch closed=new CountDownLatch(1);
            Thread writer=new Thread(()->{try {if(!h.flush())throw new AssertionError("Real held write failed");}
                catch(Throwable failure){writerFailure.set(failure);}},"actual-appops-write");
            Thread closer=new Thread(()->{try {h.close();}
                catch(Throwable failure){closeFailure.set(failure);}finally{closed.countDown();}},"actual-fixture-close");
            try {
                h.allow(); writer.start();
                check(held.entered.await(5,TimeUnit.SECONDS),"Actual AppOps serializer did not reach finishWrite");
                Path pending=Path.of(h.file.getBaseFile()+".new");
                check(Files.isRegularFile(pending),"Actual atomic temporary file does not exist at the held rename");
                closer.start();
                int lock=System.identityHashCode(get(h.writer,"mWriteLock"));
                long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5); boolean waiting=false;
                while(System.nanoTime()<deadline && closed.getCount()!=0) {
                    var info=ManagementFactory.getThreadMXBean().getThreadInfo(closer.getId());
                    if(info!=null && info.getThreadState()==Thread.State.BLOCKED
                            && info.getLockInfo()!=null && info.getLockInfo().getIdentityHashCode()==lock) {
                        waiting=true; break;
                    }
                    Thread.yield();
                }
                check(waiting,"Fixture close returned before the actual in-flight AppOps writer retired");
                check(Files.isRegularFile(pending),"Fixture teardown deleted a live writer's file");
            } finally {
                held.release.countDown();
                if(writer.getState()!=Thread.State.NEW)join(writer);
                if(closer.getState()!=Thread.State.NEW)join(closer);
                field(h.store.getClass(),"mFile").set(h.store,h.file);
            }
            check(writerFailure.get()==null,"Actual writer failed: "+writerFailure.get());
            check(closeFailure.get()==null,"Actual fixture close failed: "+closeFailure.get());
            check(h.clean(),"In-flight retirement left a dirty AppOps generation");
            check(!Files.exists(Path.of(h.file.getBaseFile()+".new")),"Retired writer left its temporary file");
        }
    }
    private static void failedWrite() throws Exception {
        try (Harness h=new Harness()) {
            Path blocker=Files.createTempFile(h.file.getBaseFile().toPath().getParent(),"owned-write-blocker-",".file");
            try {
                field(h.store.getClass(),"mFile").set(h.store,new AtomicFile(blocker.resolve("appops.xml").toFile()));
                h.allow(); boolean rejected=false;
                try { h.close(); }
                catch(AssertionError expected) {
                    rejected=expected.getMessage().contains("could not persist its final AppOps state");
                }
                check(rejected,"Fixture silently accepted failure of its owned final write");
                check(!h.clean(),"Failed real filesystem write was falsely marked durable");
            } finally {
                field(h.store.getClass(),"mFile").set(h.store,h.file);
                check(h.flush(),"Repairing the controlled filesystem boundary did not complete the real write");
                Files.delete(blocker);
            }
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        String mode=args.length>1?args[1]:"all";
        if(!java.util.Set.of("all","pending","inflight","failed").contains(mode)) {
            throw new IllegalArgumentException("Unknown persistence case: "+mode);
        }
        if(mode.equals("all")||mode.equals("pending"))pendingWrite();
        if(mode.equals("all")||mode.equals("inflight"))inFlightWrite();
        if(mode.equals("all")||mode.equals("failed"))failedWrite();
        System.out.println("Exact-alarm fixture persistence passed: "+checks+" checks; complete fixture, real policy writer/serializer and filesystem; no Windows/ART transport claim.");
    }
}
