/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Intent;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.PersistableBundle;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.List;

/** Real job authorization/queues under native Binder identities; no APK code or Windows UI. */
public final class JobSecurityTest {
    private static void require(boolean value, String reason) { if (!value) throw new AssertionError(reason); }
    private interface Operation { void run() throws Exception; }
    private static void denied(Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { return; }
        catch (InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof SecurityException) return;
            throw wrapped;
        }
        throw new AssertionError("Unauthorized job operation accepted");
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f=type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... types) throws Exception {
        Method m=type.getDeclaredMethod(name,types); m.setAccessible(true); return m;
    }
    private static Object construct(Class<?> type, Class<?>[] types, Object... values) throws Exception {
        Constructor<?> c=type.getDeclaredConstructor(types); c.setAccessible(true); return c.newInstance(values);
    }
    private static void asApp(int pid, int uid) {
        Binder.restoreCallingIdentity(((long)uid<<32)|(pid&0xffffffffL));
        require(Binder.getCallingPid()==pid && Binder.getCallingUid()==uid, "Native Binder identity injection unavailable");
    }
    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder=new Binder(); @Override public IBinder asBinder() { return binder; }
    }
    private static final class Service extends IJobService.Stub {
        int starts;
        @Override public void startJob(JobParameters params) { starts++; }
        @Override public void stopJob(JobParameters params) { }
        @Override public void onNetworkChanged(JobParameters params) { }
        @Override public void getTransferredDownloadBytes(JobParameters p, JobWorkItem w) { }
        @Override public void getTransferredUploadBytes(JobParameters p, JobWorkItem w) { }
    }
    private static File changedCode(File original, File root) throws Exception {
        byte[] bytes=Files.readAllBytes(original.toPath()); int end=-1;
        for(int i=bytes.length-22;i>=Math.max(0,bytes.length-65557);i--) {
            if((bytes[i]&255)==0x50 && (bytes[i+1]&255)==0x4b && bytes[i+2]==5 && bytes[i+3]==6) {
                int n=(bytes[i+20]&255)|((bytes[i+21]&255)<<8);
                if(i+22+n==bytes.length && n<65535) { end=i; break; }
            }
        }
        require(end>=0,"Missing fixture ZIP end record");
        byte[] changed=java.util.Arrays.copyOf(bytes,bytes.length+1);
        int size=((bytes[end+20]&255)|((bytes[end+21]&255)<<8))+1;
        changed[end+20]=(byte)size; changed[end+21]=(byte)(size>>>8); changed[changed.length-1]='x';
        root.mkdirs(); File copy=new File(root,original.getName()); Files.write(copy.toPath(),changed); return copy;
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        require(args.length==2,"Pass source-built microgprobe.apk and devsvc.apk");
        System.loadLibrary("android_runtime");
        File parent=new File(System.getenv("ANDROID_DATA")); parent.mkdirs();
        File data=Files.createTempDirectory(parent.toPath(),"job-security-").toFile();
        android.system.Os.setenv("ANDROID_DATA",data.getAbsolutePath(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        Class<?> apk=Class.forName("com.gesher.server.ApkPackage");
        Method parse=apk.getMethod("parse",String.class);
        Object probe=parse.invoke(null,args[0]), owner=parse.invoke(null,args[1]);
        Class<?> pmType=Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmType=Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> amType=Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> schedulerType=Class.forName("com.gesher.server.JobSchedulerService");
        Object pm=construct(pmType,new Class<?>[]{java.util.Collection.class},List.of(probe,owner));
        Object atm=construct(atmType,new Class<?>[]{pmType},pm);
        IJobScheduler jobs=(IJobScheduler)construct(schedulerType,new Class<?>[]{amType,atmType,pmType},null,atm,pm);
        int uid=(Integer)apk.getMethod("uid").invoke(probe), ownerUid=(Integer)apk.getMethod("uid").invoke(owner);
        int pid=888897;
        Class<?> processType=Class.forName(atmType.getName()+"$ProcessRecord");
        long original=Binder.clearCallingIdentity();
        Object process;
        try {
            asApp(pid,uid);
            process=construct(processType,new Class<?>[]{atmType,int.class,IApplicationThread.class},atm,pid,new AppThread());
        } finally { Binder.restoreCallingIdentity(original); }
        field(processType,"packageName").set(process,"com.gesher.microgprobe");
        field(processType,"ready").setBoolean(process,true);
        ((List<Object>)field(atmType,"mProcesses").get(atm)).add(process);
        String ns="victim-namespace", own="com.gesher.microgprobe", victim="com.gesher.devsvc";
        PersistableBundle extras=new PersistableBundle(); extras.putString("marker","victim-original");
        JobInfo victimJob=new JobInfo.Builder(87314,new ComponentName(victim,victim+".TestJobService"))
                .setMinimumLatency(3600000).setExtras(extras).build();
        JobInfo ownJob=new JobInfo.Builder(87314,new ComponentName(own,own+".ProbeJobService"))
                .setMinimumLatency(3600000).build();
        try {
            require(jobs.schedule(ns,victimJob)==JobScheduler.RESULT_SUCCESS,"System could not seed victim job");
            asApp(pid,uid);
            denied(()->jobs.schedule(ns,victimJob));
            denied(()->jobs.enqueue(ns,victimJob,new JobWorkItem(new Intent("forged.work"))));
            denied(()->jobs.scheduleAsPackage(ns,victimJob,victim,0,"forged"));
            denied(()->jobs.scheduleAsPackage(ns,ownJob,own,0,"self-admin"));
            denied(()->jobs.getStartedJobs()); denied(()->jobs.getAllJobSnapshots());
            denied(()->jobs.registerUserVisibleJobObserver(null));
            denied(()->jobs.unregisterUserVisibleJobObserver(null));
            denied(()->jobs.notePendingUserRequestedAppStop(victim,0,"forged"));
            denied(()->jobs.hasRunUserInitiatedJobsPermission(victim,0));
            denied(()->jobs.hasRunUserInitiatedJobsPermission(own,1));
            denied(()->jobs.canRunUserInitiatedJobs(victim));
            require(jobs.getPendingJob(ns,87314)==null && jobs.getAllPendingJobs().isEmpty()
                    && jobs.getAllPendingJobsInNamespace(ns).getList().isEmpty(),"Victim job leaked to app queries");
            jobs.cancel(ns,87314); jobs.cancelAllInNamespace(ns); jobs.cancelAll();
            require(jobs.schedule(ns,ownJob)==JobScheduler.RESULT_SUCCESS,"Own valid JobService rejected");
            require(jobs.enqueue(ns,ownJob,new JobWorkItem(new Intent("own.work")))==JobScheduler.RESULT_SUCCESS,
                    "Own enqueued work rejected");
            require(jobs.getPendingJob(ns,87314)!=null,"Own pending job hidden");
            jobs.cancelAllInNamespace(ns);
            require(jobs.getPendingJob(ns,87314)==null,"Own namespace cancel failed");
            JobInfo persisted=new JobInfo.Builder(87315,ownJob.getService()).setMinimumLatency(3600000).setPersisted(true).build();
            denied(()->jobs.schedule(ns,persisted));
            System.out.println("JobSecurityTest: foreign scheduling/admin denied; own schedule/enqueue/cancel authorized");

            asApp(888899,uid); // Unattached remote PID with an otherwise real app UID.
            denied(()->jobs.schedule(ns,ownJob));
            denied(()->jobs.enqueue(ns,ownJob,new JobWorkItem(new Intent("unknown.work"))));
            denied(()->jobs.cancel(ns,87314)); denied(()->jobs.cancelAll());
            denied(()->jobs.cancelAllInNamespace(ns)); denied(()->jobs.getAllPendingJobs());
            denied(()->jobs.getAllPendingJobsInNamespace(ns)); denied(()->jobs.getPendingJob(ns,87314));
            denied(()->jobs.getPendingJobReason(ns,87314)); denied(()->jobs.getPendingJobReasons(ns,87314));
            denied(()->jobs.getPendingJobReasonsHistory(ns,87314));
            asApp(pid,ownerUid); // Known PID cannot claim another installed UID.
            denied(()->jobs.cancelAll()); denied(()->jobs.getAllPendingJobs());
            Binder.restoreCallingIdentity(original);
            JobInfo unchanged=jobs.getPendingJob(ns,87314);
            require(unchanged!=null && victim.equals(unchanged.getService().getPackageName())
                            && "victim-original".equals(unchanged.getExtras().getString("marker")),
                    "Unauthorized cancel/enqueue mutated the victim's original job");
            denied(()->jobs.scheduleAsPackage(ns,victimJob,victim,1,"wrong-user"));
            denied(()->jobs.scheduleAsPackage(ns,victimJob,own,0,"wrong-attribution"));
            System.out.println("JobSecurityTest: unknown PID/UID mismatch and namespace/global access fail closed; victim untouched");

            Class<?> jobType=Class.forName(schedulerType.getName()+"$Job");
            Class<?> runType=Class.forName(schedulerType.getName()+"$Run");
            Object victimRecord=((List<?>)field(schedulerType,"mJobs").get(jobs)).get(0);
            Object run=construct(runType,new Class<?>[]{schedulerType,jobType},jobs,victimRecord);
            field(jobType,"run").set(victimRecord,run); field(schedulerType,"mRunning").setInt(jobs,1);
            IJobCallback callback=(IJobCallback)run;
            asApp(pid,uid);
            denied(()->callback.dequeueWork(87314)); denied(()->callback.completeWork(87314,1));
            denied(()->callback.jobFinished(87314,false));
            Binder.restoreCallingIdentity(original);
            File replaced=changedCode(new File(args[1]),new File(data,"changed-owner"));
            method(pmType,"add",apk).invoke(pm,parse.invoke(null,replaced.getPath()));
            Service service=new Service();
            method(runType,"started",IBinder.class).invoke(run,service.asBinder());
            require(service.starts==0 && field(runType,"params").get(run)==null
                            && field(runType,"done").getBoolean(run),
                    "An owner replacement during pending binding received old job parameters");
            require(((List<?>)field(schedulerType,"mJobs").get(jobs)).isEmpty(),"Changed-owner job was retained");
            System.out.println("JobSecurityTest: callback theft and pending-bind replacement cannot expose job work/parameters");
        } finally {
            Binder.restoreCallingIdentity(original);
            Thread thread=(Thread)field(schedulerType,"mThread").get(jobs);
            if(thread!=null) thread.interrupt();
        }
        System.out.println("JobSecurityTest all passed");
    }
}
