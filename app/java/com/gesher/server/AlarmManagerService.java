/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.AlarmManager;
import android.app.IAlarmCompleteListener;
import android.app.IAlarmListener;
import android.app.IAlarmManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.SystemClock;
import android.os.WorkSource;

import java.util.ArrayList;
import java.util.Calendar;
import java.util.List;
import java.util.TimeZone;

/**
 * {@code alarm}: AlarmManagerService on Windows (devsvc track, docs/services.md "Device
 * services", ADR 0038). Alarms of the four types (RTC and ELAPSED_REALTIME, with or
 * without _WAKEUP), exact, windowed or inexact (delivered at the start of their window:
 * nothing is batched), repeating ({@code setRepeating}, {@code setInexactRepeating}: missed
 * periods are delivered once, with {@link Intent#EXTRA_ALARM_COUNT}), alarm clocks
 * ({@code setAlarmClock}, {@code getNextAlarmClock}, ACTION_NEXT_ALARM_CLOCK_CHANGED),
 * replaced by a new {@code set} with the same PendingIntent or listener, cancelled by
 * {@code remove}/{@code removeAll}. Delivery sends the PendingIntent as its creator would
 * (a broadcast, a service or an activity: the activity manager starts the package's
 * process if none runs) or calls the app's {@code OnAlarmListener} in its process.
 *
 * <p>One thread waits for the next alarm on a Windows waitable timer
 * ({@link WindowsHost#alarmWait}): an absolute wall-clock time, so RTC alarms follow clock
 * changes, and an alarm whose time passed while Windows slept fires on resume; a _WAKEUP
 * alarm's timer may wake Windows from sleep (where wake timers are allowed). The thread
 * also notices clock and time zone changes (WM_TIMECHANGE, WM_SETTINGCHANGE, a jump of the wall clock
 * against the monotonic one) and the local date's change, and sends TIME_SET,
 * TIMEZONE_CHANGED and DATE_CHANGED to each package (registered and manifest receivers).
 * There are no idle or doze restrictions, and exact alarms are allowed to every app.
 *
 * <p>Alarms live in the device: they keep it running while one is due within its idle
 * timeout ({@link DeviceWork}) and are lost when it stops, as on a reboot.
 */
final class AlarmManagerService extends IAlarmManager.Default
        implements DeviceWork.Source, WindowsHost.Listener {
    /** The longest wait without looking at the clocks again. */
    private static final int CHECK_MS = 60_000;
    /** A wall clock jump against the monotonic clock beyond this is a TIME_SET. */
    private static final long TIME_SET_SLACK_MS = 2_000;

    private final ActivityManagerService mAm;
    private final PackageManagerService mPm;

    /** An alarm: AlarmManagerService's Alarm. */
    private static final class Alarm {
        final String packageName;
        final int type;
        long when;  // in its type's clock (wall clock for RTC, elapsedRealtime otherwise)
        final long windowLength;
        final long interval;
        final int flags;
        final PendingIntent operation;
        final IAlarmListener listener;
        final String tag;
        final AlarmManager.AlarmClockInfo alarmClock;
        int count;  // periods passed when delivered

        Alarm(String packageName, int type, long when, long windowLength, long interval,
                int flags, PendingIntent operation, IAlarmListener listener, String tag,
                AlarmManager.AlarmClockInfo alarmClock) {
            this.packageName = packageName;
            this.type = type;
            this.when = when;
            this.windowLength = windowLength;
            this.interval = interval;
            this.flags = flags;
            this.operation = operation;
            this.listener = listener;
            this.tag = tag;
            this.alarmClock = alarmClock;
        }

        boolean rtc() {
            return type == AlarmManager.RTC || type == AlarmManager.RTC_WAKEUP;
        }

        boolean wakeup() {
            return type == AlarmManager.RTC_WAKEUP || type == AlarmManager.ELAPSED_REALTIME_WAKEUP;
        }

        /** Its time on the elapsedRealtime clock, given the clocks now. */
        long elapsed(long nowElapsed, long nowRtc) {
            return rtc() ? nowElapsed + (when - nowRtc) : when;
        }

        boolean matches(PendingIntent op, IAlarmListener l) {
            if (op != null) return operation != null && operation.equals(op);
            return l != null && listener != null && listener.asBinder() == l.asBinder();
        }

        @Override
        public String toString() {
            String what = operation != null ? "PendingIntent " + operation
                    : "listener " + (tag != null ? tag : "");
            return typeName(type) + " alarm of " + packageName + " (" + what + ")"
                    + (interval > 0 ? ", every " + interval + " ms" : "")
                    + (alarmClock != null ? ", alarm clock" : "");
        }
    }

    // Guarded by this.
    private final List<Alarm> mAlarms = new ArrayList<>();
    private Thread mThread;
    private long mClockOffset;    // currentTimeMillis - elapsedRealtime, as last seen
    private String mWindowsZone;  // Windows' time zone key name, as last seen
    private long mLocalDay = -1;  // the local date, as last seen (days since the epoch)
    private AlarmManager.AlarmClockInfo mNextAlarmClock;
    private boolean mTimeEvent;   // WM_TIMECHANGE came: the time was set
    private boolean mSuspended;   // Windows goes to sleep (PBT_APMSUSPEND) until it resumes
    private long mSuspendedAt;    // uptimeMillis of PBT_APMSUSPEND
    /** Still running this long after PBT_APMSUSPEND: Windows did not sleep after all. */
    private static final long SUSPEND_GIVE_UP_MS = 120_000;

    AlarmManagerService(ActivityManagerService am, PackageManagerService pm) {
        mAm = am;
        mPm = pm;
        DeviceWork.add(this);
        // The clock watch (TIME_SET, TIMEZONE_CHANGED, DATE_CHANGED) from the system server's
        // start; in an app's own process (--single-process) from its first alarm.
        if (WindowsHost.inSystemServer()) start();
    }

    /** Starts the alarm thread and the clock watch (on first use, or by the system server). */
    synchronized void start() {
        if (mThread != null) return;
        mClockOffset = System.currentTimeMillis() - SystemClock.elapsedRealtime();
        mWindowsZone = WindowsHost.timeZoneKeyName();
        mLocalDay = localDay(System.currentTimeMillis());
        WindowsHost.addListener(this);
        mThread = new Thread(this::run, "alarms");
        mThread.setDaemon(true);
        mThread.start();
        SystemServer.log("alarm: waiting on a Windows timer"
                + (WindowsHost.available() ? "" : " (no natives: a Java wait)")
                + "; Windows time zone " + mWindowsZone + " (" + olsonZone() + ")");
    }

    // --- IAlarmManager -------------------------------------------------------------------

    @Override
    public void set(String callingPackage, int type, long triggerAtTime, long windowLength,
            long interval, int flags, PendingIntent operation, IAlarmListener listener,
            String listenerTag, WorkSource workSource, AlarmManager.AlarmClockInfo alarmClock) {
        if (operation == null && listener == null) {
            SystemServer.log("alarm: set from " + callingPackage + " with no operation");
            return;
        }
        if (type < AlarmManager.RTC_WAKEUP || type > AlarmManager.ELAPSED_REALTIME) {
            throw new IllegalArgumentException("Invalid alarm type " + type);
        }
        start();
        Alarm a = new Alarm(callingPackage, type, Math.max(0, triggerAtTime), windowLength,
                Math.max(0, interval), flags, operation, listener, listenerTag, alarmClock);
        synchronized (this) {
            mAlarms.removeIf(old -> old.matches(operation, listener));
            mAlarms.add(a);
        }
        SystemServer.log("alarm: set " + a + " in " + inMs(a) + " ms");
        if (alarmClock != null) alarmClocksChanged();
        WindowsHost.alarmKick();
    }

    @Override
    public void remove(PendingIntent operation, IAlarmListener listener) {
        boolean clock;
        int removed;
        synchronized (this) {
            int before = mAlarms.size();
            clock = mAlarms.stream().anyMatch(a -> a.alarmClock != null
                    && a.matches(operation, listener));
            mAlarms.removeIf(a -> a.matches(operation, listener));
            removed = before - mAlarms.size();
        }
        if (removed > 0) {
            SystemServer.log("alarm: removed " + removed + " alarm(s) of "
                    + (operation != null ? operation : "a listener"));
            if (clock) alarmClocksChanged();
            WindowsHost.alarmKick();
        }
    }

    @Override
    public void removeAll(String packageName) {
        synchronized (this) {
            mAlarms.removeIf(a -> a.packageName != null && a.packageName.equals(packageName));
        }
        alarmClocksChanged();
        WindowsHost.alarmKick();
    }

    @Override
    public boolean setTime(long millis) {
        // Setting Windows' clock is the user's (and needs an administrator), not an app's.
        SystemServer.log("alarm: setTime(" + millis + ") refused: Windows keeps the time");
        return false;
    }

    @Override
    public void setTimeZone(String zone) {
        SystemServer.log("alarm: setTimeZone(" + zone + ") ignored: Windows keeps the zone");
    }

    @Override
    public long getNextWakeFromIdleTime() {
        return Long.MAX_VALUE;  // no device idle mode
    }

    @Override
    public synchronized AlarmManager.AlarmClockInfo getNextAlarmClock(int userId) {
        return nextAlarmClockLocked();
    }

    @Override
    public boolean canScheduleExactAlarms(String packageName) {
        return true;
    }

    @Override
    public boolean hasScheduleExactAlarm(String packageName, int userId) {
        return true;
    }

    @Override
    public int getConfigVersion() {
        return 0;
    }

    // --- DeviceWork.Source ------------------------------------------------------------------

    @Override
    public synchronized long nextDueElapsed() {
        long nowElapsed = SystemClock.elapsedRealtime();
        long nowRtc = System.currentTimeMillis();
        long next = Long.MAX_VALUE;
        for (Alarm a : mAlarms) next = Math.min(next, a.elapsed(nowElapsed, nowRtc));
        return next;
    }

    // --- the alarm thread ---------------------------------------------------------------------

    @Override
    public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_SUSPENDING) != 0) {
            // As on a phone going to sleep: only _WAKEUP alarms fire until it is awake;
            // the others are delivered when it resumes (Windows does not run the device
            // while it sleeps; a _WAKEUP alarm's timer may wake it).
            synchronized (this) {
                mSuspended = true;
                mSuspendedAt = SystemClock.uptimeMillis();
            }
            SystemServer.log("alarm: Windows goes to sleep: only _WAKEUP alarms until it resumes");
        }
        if ((events & WindowsHost.EVENT_RESUMED) != 0) {
            synchronized (this) {
                mSuspended = false;
            }
        }
        if ((events & (WindowsHost.EVENT_TIME_CHANGED | WindowsHost.EVENT_RESUMED
                | WindowsHost.EVENT_SETTING_CHANGED)) != 0) {
            if ((events & WindowsHost.EVENT_TIME_CHANGED) != 0) {
                synchronized (this) {
                    mTimeEvent = true;
                }
            }
            if ((events & WindowsHost.EVENT_RESUMED) != 0) {
                SystemServer.log("alarm: Windows resumed from sleep");
            }
            WindowsHost.alarmKick();  // look at the clocks and the alarms again
        }
    }

    private void run() {
        while (true) {
            try {
                checkClocks();
                synchronized (this) {
                    if (mSuspended && SystemClock.uptimeMillis() - mSuspendedAt > SUSPEND_GIVE_UP_MS) {
                        mSuspended = false;
                        SystemServer.log("alarm: no resume came after the suspend: alarms as usual");
                    }
                }
                List<Alarm> due = new ArrayList<>();
                long nextRtc;
                boolean wakeup = false;
                synchronized (this) {
                    long nowElapsed = SystemClock.elapsedRealtime();
                    long nowRtc = System.currentTimeMillis();
                    for (int i = 0; i < mAlarms.size(); i++) {
                        Alarm a = mAlarms.get(i);
                        long at = a.elapsed(nowElapsed, nowRtc);
                        if (at > nowElapsed || (mSuspended && !a.wakeup())) continue;
                        a.count = 1;
                        if (a.interval > 0) {
                            // Missed periods count, and the next one is in the future.
                            long late = nowElapsed - at;
                            long periods = late / a.interval;
                            a.count += (int) Math.min(Integer.MAX_VALUE - 1, periods);
                            a.when += (periods + 1) * a.interval;
                        } else {
                            mAlarms.remove(i--);
                        }
                        due.add(a);
                    }
                    long nextElapsed = Long.MAX_VALUE;
                    for (Alarm a : mAlarms) {
                        if (mSuspended && !a.wakeup()) continue;  // after the resume
                        long at = a.elapsed(nowElapsed, nowRtc);
                        if (at < nextElapsed) {
                            nextElapsed = at;
                            wakeup = a.wakeup();
                        }
                    }
                    // The next local midnight is DATE_CHANGED's.
                    long midnight = nextLocalMidnight(nowRtc);
                    long midnightElapsed = nowElapsed + (midnight - nowRtc);
                    if (midnightElapsed < nextElapsed) {
                        nextElapsed = midnightElapsed;
                        wakeup = false;
                    }
                    nextRtc = nowRtc + (nextElapsed - nowElapsed);
                }
                if (!due.isEmpty()) {
                    boolean clocks = false;
                    for (Alarm a : due) {
                        deliver(a);
                        clocks |= a.alarmClock != null && a.interval == 0;
                    }
                    if (clocks) alarmClocksChanged();
                    continue;  // delivering took time: look again
                }
                boolean suspended;
                synchronized (this) {
                    suspended = mSuspended;
                }
                WindowsHost.alarmWait(nextRtc, wakeup, suspended ? 10_000 : CHECK_MS);
            } catch (Throwable t) {
                SystemServer.log("alarm: the alarm thread failed: " + t);
                t.printStackTrace();
                SystemClock.sleep(1000);
            }
        }
    }

    private void deliver(Alarm a) {
        SystemServer.log("alarm: firing " + a + (a.count > 1 ? " (" + a.count + " periods)" : ""));
        if (a.operation != null) {
            Intent fillIn = new Intent().putExtra(Intent.EXTRA_ALARM_COUNT, a.count);
            int result;
            try {
                result = mAm.sendIntentSender(null, a.operation.getTarget(), null, 0, fillIn,
                        null, null, null, null);
            } catch (Throwable t) {
                SystemServer.log("alarm: sending " + a.operation + " failed: " + t);
                return;
            }
            if (result == ActivityManager.START_CANCELED) {
                // The PendingIntent was cancelled: so is the alarm (AlarmManagerService
                // removes alarms whose operation is cancelled).
                SystemServer.log("alarm: " + a.operation + " was cancelled; alarm removed");
                synchronized (this) {
                    mAlarms.remove(a);
                }
            }
        } else {
            try {
                a.listener.doAlarm(new IAlarmCompleteListener.Stub() {
                    @Override
                    public void alarmComplete(IBinder who) {
                    }
                });
            } catch (RemoteException e) {
                // The app's process is gone, and its listener with it.
                SystemServer.log("alarm: the listener of " + a.packageName + " is gone; alarm"
                        + " removed");
                synchronized (this) {
                    mAlarms.remove(a);
                }
            }
        }
    }

    // --- clocks -------------------------------------------------------------------------------

    /** TIME_SET, TIMEZONE_CHANGED and DATE_CHANGED, as the clocks show them. */
    private void checkClocks() {
        long nowRtc = System.currentTimeMillis();
        long offset = nowRtc - SystemClock.elapsedRealtime();
        String zone = WindowsHost.timeZoneKeyName();
        boolean timeSet;
        boolean zoneChanged;
        boolean dateChanged;
        synchronized (this) {
            zoneChanged = zone != null && !zone.equals(mWindowsZone);
            // WM_TIMECHANGE without a new zone is the time being set (even to what it
            // was, as Android's TIME_SET); a jump of the wall clock without it too.
            timeSet = Math.abs(offset - mClockOffset) > TIME_SET_SLACK_MS
                    || (mTimeEvent && !zoneChanged);
            mTimeEvent = false;
            mClockOffset = offset;
            if (zone != null) mWindowsZone = zone;
            long day = localDay(nowRtc);
            dateChanged = mLocalDay >= 0 && day != mLocalDay;
            mLocalDay = day;
        }
        if (zoneChanged) {
            String olson = olsonZone();
            SystemServer.log("alarm: Windows' time zone is now " + zone + " (" + olson + ")");
            // Processes read the default zone again, as ActivityManagerService's
            // UPDATE_TIME_ZONE has every app process do (hwsvc2, ADR 0051).
            TimeZone.setDefault(null);
            updateTimeZones();
            Intent i = new Intent(Intent.ACTION_TIMEZONE_CHANGED);
            i.putExtra(Intent.EXTRA_TIMEZONE, olson);
            broadcastToAll(i);
        }
        if (timeSet) {
            SystemServer.log("alarm: the wall clock was set (" + nowRtc + ")");
            broadcastToAll(new Intent(Intent.ACTION_TIME_CHANGED));
        }
        if (dateChanged) {
            SystemServer.log("alarm: the date changed");
            // Not one of the broadcasts manifest receivers still get (Android 8's implicit
            // broadcast limits): registered receivers only.
            broadcast(new Intent(Intent.ACTION_DATE_CHANGED));
        }
    }

    /** IApplicationThread.updateTimeZone to every app process (TimeZone.setDefault(null)). */
    private static void updateTimeZones() {
        Services.Created c = Services.sLast;
        if (c == null) return;
        for (ActivityTaskManagerService.ProcessRecord p : c.atm.processes()) {
            if (p.thread == null) continue;
            try {
                p.thread.updateTimeZone();
            } catch (Throwable t) {
                // the process is gone
            }
        }
    }

    /** The Olson id of Windows' time zone (ICU's windowsZones), else the default's. */
    private String olsonZone() {
        String windows;
        synchronized (this) {
            windows = mWindowsZone;
        }
        if (windows != null) {
            try {
                String id = android.icu.util.TimeZone.getIDForWindowsID(windows, null);
                if (id != null) return id;
            } catch (Throwable t) {
                // no ICU mapping
            }
            // The runtime's ICU data may have no windowsZones: Windows' own ICU has.
            String id = WindowsHost.ianaZone(windows);
            if (id != null) return id;
        }
        return TimeZone.getDefault().getID();
    }

    private long localDay(long rtc) {
        TimeZone tz = TimeZone.getTimeZone(olsonZone());
        return Math.floorDiv(rtc + tz.getOffset(rtc), 86_400_000L);
    }

    private long nextLocalMidnight(long rtc) {
        Calendar c = Calendar.getInstance(TimeZone.getTimeZone(olsonZone()));
        c.setTimeInMillis(rtc);
        c.set(Calendar.HOUR_OF_DAY, 0);
        c.set(Calendar.MINUTE, 0);
        c.set(Calendar.SECOND, 0);
        c.set(Calendar.MILLISECOND, 0);
        c.add(Calendar.DAY_OF_MONTH, 1);
        return c.getTimeInMillis();
    }

    /**
     * Sends {@code intent} to every package: its registered receivers and manifest
     * receivers (BroadcastController delivers implicit broadcasts to manifest receivers of
     * the package they name), as the system sends TIME_SET, TIMEZONE_CHANGED and
     * NEXT_ALARM_CLOCK_CHANGED, which are exempt from the implicit broadcast limits.
     */
    private void broadcastToAll(Intent intent) {
        intent.addFlags(Intent.FLAG_RECEIVER_REPLACE_PENDING);
        for (ApkPackage p : new ArrayList<>(mPm.packages())) {
            Intent i = new Intent(intent);
            i.setPackage(p.packageName);
            try {
                mAm.sendBroadcastFor("android", i, null);
            } catch (Throwable t) {
                SystemServer.log("alarm: " + intent.getAction() + " to " + p.packageName
                        + " failed: " + t);
            }
        }
    }

    /** Sends {@code intent} to the registered receivers of every package. */
    private void broadcast(Intent intent) {
        intent.addFlags(Intent.FLAG_RECEIVER_REPLACE_PENDING);
        try {
            mAm.sendBroadcastFor("android", intent, null);
        } catch (Throwable t) {
            SystemServer.log("alarm: " + intent.getAction() + " failed: " + t);
        }
    }

    private AlarmManager.AlarmClockInfo nextAlarmClockLocked() {
        AlarmManager.AlarmClockInfo next = null;
        for (Alarm a : mAlarms) {
            if (a.alarmClock == null) continue;
            if (next == null || a.alarmClock.getTriggerTime() < next.getTriggerTime()) {
                next = a.alarmClock;
            }
        }
        return next;
    }

    private void alarmClocksChanged() {
        boolean changed;
        synchronized (this) {
            AlarmManager.AlarmClockInfo next = nextAlarmClockLocked();
            changed = next != mNextAlarmClock;
            mNextAlarmClock = next;
        }
        if (changed) broadcastToAll(new Intent(AlarmManager.ACTION_NEXT_ALARM_CLOCK_CHANGED));
    }

    private static long inMs(Alarm a) {
        return a.rtc() ? a.when - System.currentTimeMillis()
                : a.when - SystemClock.elapsedRealtime();
    }

    static String typeName(int type) {
        switch (type) {
            case AlarmManager.RTC_WAKEUP: return "RTC_WAKEUP";
            case AlarmManager.RTC: return "RTC";
            case AlarmManager.ELAPSED_REALTIME_WAKEUP: return "ELAPSED_REALTIME_WAKEUP";
            case AlarmManager.ELAPSED_REALTIME: return "ELAPSED_REALTIME";
            default: return "type " + type;
        }
    }
}
