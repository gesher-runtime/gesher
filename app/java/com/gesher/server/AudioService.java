/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.AudioPlaybackConfiguration;
import android.media.AudioSystem;
import android.media.IAudioFocusDispatcher;
import android.media.IAudioService;
import android.media.IPlaybackConfigDispatcher;
import android.media.PlayerBase;
import android.media.audiopolicy.AudioVolumeGroup;
import android.media.audiopolicy.IAudioPolicyCallback;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * audio: AudioService for the Gesher device (docs/media.md, ADR 0042).
 *
 * <p>Stream volumes, mutes and the master volume live in the audio state every process of
 * the device shares (compat/media/audio/shared_state.h), written through AudioSystem's
 * natives as AudioService writes AudioFlinger's on a phone; each app process's audio engine
 * plays at them, as the volume of the Windows audio session of the stream's group, and the
 * Windows volume mixer's changes come back there. Streams follow the phone's volume aliases
 * (system, DTMF and enforced sounds at the ring volume, TTS, accessibility and the assistant
 * at the media volume). The ringer mode mutes the streams it affects. Audio focus is a stack
 * across the device's apps: a request takes focus from the holder (a loss, transient or
 * not, through its IAudioFocusDispatcher), an abandon gives it back to the one below.
 */
public final class AudioService extends IAudioService.Default {
    private static final String TAG = "GesherAudioService";

    /** AudioService's STREAM_VOLUME_ALIAS_DEFAULT (a phone). */
    static final int[] ALIAS = {
        AudioSystem.STREAM_VOICE_CALL,  // VOICE_CALL
        AudioSystem.STREAM_RING,        // SYSTEM
        AudioSystem.STREAM_RING,        // RING
        AudioSystem.STREAM_MUSIC,       // MUSIC
        AudioSystem.STREAM_ALARM,       // ALARM
        AudioSystem.STREAM_NOTIFICATION,  // NOTIFICATION
        AudioSystem.STREAM_VOICE_CALL,  // BLUETOOTH_SCO
        AudioSystem.STREAM_RING,        // SYSTEM_ENFORCED
        AudioSystem.STREAM_RING,        // DTMF
        AudioSystem.STREAM_MUSIC,       // TTS
        AudioSystem.STREAM_MUSIC,       // ACCESSIBILITY
        AudioSystem.STREAM_MUSIC,       // ASSISTANT
    };
    /** AudioService's MAX_STREAM_VOLUME and MIN_STREAM_VOLUME. */
    static final int[] MAX = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
    static final int[] MIN = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    /** Streams the ringer mode silences (AudioService's default ringer-affected streams). */
    static final int RINGER_AFFECTED = (1 << AudioSystem.STREAM_RING)
            | (1 << AudioSystem.STREAM_NOTIFICATION) | (1 << AudioSystem.STREAM_SYSTEM)
            | (1 << AudioSystem.STREAM_DTMF);

    private final Object mLock = new Object();
    private final boolean[] mMuted = new boolean[ALIAS.length];  // by the user (adjust MUTE)
    private int mRingerMode = AudioManager.RINGER_MODE_NORMAL;
    private int mMode = AudioManager.MODE_NORMAL;
    private int mNextPiid = 1;

    /**
     * The default sounds, as settings (RINGTONE, NOTIFICATION_SOUND, ALARM_ALERT): Gesher's
     * own in $ANDROID_ROOT/media/audio (compat/media/sounds), as file URIs. RingtoneManager
     * and MediaPlayer.setDataSource(Context, Uri) resolve the default URIs to them; where a
     * file is missing the setting stays unset and Ringtone falls back to framework-res's
     * fallbackring. SettingsProvider fills its system table with these.
     */
    static void putDefaultSounds(java.util.Map<String, String> system) {
        java.io.File audio = new java.io.File(android.os.Environment.getRootDirectory(),
                "media/audio");
        String[][] sounds = {
            {android.provider.Settings.System.RINGTONE, "ringtones/Gesher_Ring.ogg"},
            {android.provider.Settings.System.NOTIFICATION_SOUND,
                    "notifications/Gesher_Notification.ogg"},
            {android.provider.Settings.System.ALARM_ALERT, "alarms/Gesher_Alarm.ogg"},
        };
        for (String[] s : sounds) {
            java.io.File f = new java.io.File(audio, s[1]);
            if (f.isFile()) system.put(s[0], android.net.Uri.fromFile(f).toString());
        }
    }

    public AudioService() {
        for (int s = 0; s < ALIAS.length; s++) {
            AudioSystem.initStreamVolume(s, MIN[s], MAX[s]);
        }
    }

    /**
     * Whether this process's app may record audio (media3, ADR 0059): its manifest asks for
     * android.permission.RECORD_AUDIO (Gesher grants what a manifest asks for, as it does
     * the camera's; docs/devices.md). AudioRecord, AAudio and OpenSL ES input and
     * MediaRecorder ask this (their natives, compat/media) before they capture, as
     * AudioFlinger checks the record permission on a phone. A process without an app
     * (a tool on the boot class path) may.
     */
    public static boolean recordAudioAllowed() {
        String pkg = android.app.ActivityThread.currentPackageName();
        if (pkg == null) return true;
        try {
            android.content.pm.PackageInfo info = android.app.ActivityThread.getPackageManager()
                    .getPackageInfo(pkg, android.content.pm.PackageManager.GET_PERMISSIONS, 0);
            if (info == null || info.requestedPermissions == null) return false;
            for (String p : info.requestedPermissions) {
                if (android.Manifest.permission.RECORD_AUDIO.equals(p)) return true;
            }
            Log.w(TAG, pkg + " does not have " + android.Manifest.permission.RECORD_AUDIO);
            return false;
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "checking " + pkg + "'s permissions: " + e);
            return false;
        }
    }

    static boolean valid(int stream) {
        return stream >= 0 && stream < ALIAS.length;
    }

    /** The index a stream plays at: its alias's. */
    private int index(int stream) {
        return AudioSystem.getStreamVolumeIndex(ALIAS[stream], AudioSystem.DEVICE_OUT_DEFAULT);
    }

    private boolean silencedByRinger(int stream) {
        return mRingerMode != AudioManager.RINGER_MODE_NORMAL
                && (RINGER_AFFECTED & (1 << stream)) != 0;
    }

    /** Writes the alias group's index and mutes to the shared state. */
    private void apply(int alias, int index) {
        for (int s = 0; s < ALIAS.length; s++) {
            if (ALIAS[s] != alias) continue;
            boolean muted = mMuted[alias] || silencedByRinger(s);
            AudioSystem.setStreamVolumeIndexAS(s, Math.max(MIN[s], Math.min(MAX[s], index)), muted,
                    AudioSystem.DEVICE_OUT_DEFAULT);
        }
    }

    // --- Volumes ---------------------------------------------------------------------------

    @Override
    public int getStreamVolume(int streamType) {
        if (!valid(streamType)) return 0;
        synchronized (mLock) {
            int alias = ALIAS[streamType];
            if (mMuted[alias] || silencedByRinger(streamType)) return 0;
            return index(streamType);
        }
    }

    @Override
    public int getStreamMaxVolume(int streamType) {
        return valid(streamType) ? MAX[ALIAS[streamType]] : 0;
    }

    @Override
    public int getStreamMinVolume(int streamType) {
        return valid(streamType) ? MIN[ALIAS[streamType]] : 0;
    }

    @Override
    public int getLastAudibleStreamVolume(int streamType) {
        return valid(streamType) ? index(streamType) : 0;
    }

    @Override
    public void setStreamVolume(int streamType, int index, int flags, String callingPackage) {
        if (!valid(streamType)) throw new IllegalArgumentException("Bad stream type " + streamType);
        synchronized (mLock) {
            int alias = ALIAS[streamType];
            int max = MAX[alias];
            if (index < MIN[alias] || index > max) {
                throw new IllegalArgumentException("Bad volume index " + index);
            }
            if (index == 0 && MIN[alias] == 0) {
                // Volume 0 mutes the stream and keeps its last audible index (AudioService's).
                mMuted[alias] = true;
                apply(alias, index(streamType));
            } else {
                mMuted[alias] = false;
                apply(alias, index);
            }
            Log.i(TAG, "stream " + streamType + " volume " + index + "/" + max + " (by "
                    + callingPackage + ")");
        }
    }

    @Override
    public void setStreamVolumeWithAttribution(int streamType, int index, int flags,
            String callingPackage, String attributionTag) {
        setStreamVolume(streamType, index, flags, callingPackage);
    }

    @Override
    public void adjustStreamVolume(int streamType, int direction, int flags,
            String callingPackage) {
        if (!valid(streamType)) return;
        synchronized (mLock) {
            int alias = ALIAS[streamType];
            int index = index(streamType);
            switch (direction) {
                case AudioManager.ADJUST_RAISE:
                    if (mMuted[alias]) {
                        mMuted[alias] = false;
                    } else {
                        index = Math.min(MAX[alias], index + 1);
                    }
                    break;
                case AudioManager.ADJUST_LOWER:
                    if (index - 1 < Math.max(MIN[alias], 1) && MIN[alias] == 0) {
                        mMuted[alias] = true;
                    } else {
                        index = Math.max(MIN[alias], index - 1);
                    }
                    break;
                case AudioManager.ADJUST_MUTE:
                    mMuted[alias] = true;
                    break;
                case AudioManager.ADJUST_UNMUTE:
                    mMuted[alias] = false;
                    break;
                case AudioManager.ADJUST_TOGGLE_MUTE:
                    mMuted[alias] = !mMuted[alias];
                    break;
                default:
                    return;
            }
            apply(alias, index);
        }
    }

    @Override
    public void adjustStreamVolumeWithAttribution(int streamType, int direction, int flags,
            String callingPackage, String attributionTag) {
        adjustStreamVolume(streamType, direction, flags, callingPackage);
    }

    @Override
    public void adjustSuggestedStreamVolume(int direction, int suggestedStreamType, int flags) {
        adjustStreamVolume(valid(suggestedStreamType) ? suggestedStreamType
                : AudioSystem.STREAM_MUSIC, direction, flags, null);
    }

    @Override
    public void adjustVolume(int direction, int flags) {
        adjustStreamVolume(AudioSystem.STREAM_MUSIC, direction, flags, null);
    }

    @Override
    public boolean isStreamMute(int streamType) {
        if (!valid(streamType)) return false;
        synchronized (mLock) {
            return mMuted[ALIAS[streamType]] || silencedByRinger(streamType);
        }
    }

    @Override
    public boolean isStreamAffectedByRingerMode(int streamType) {
        return valid(streamType) && (RINGER_AFFECTED & (1 << streamType)) != 0;
    }

    @Override
    public boolean isStreamAffectedByMute(int streamType) {
        return valid(streamType);
    }

    @Override
    public boolean isMasterMute() {
        return AudioSystem.getMasterMute();
    }

    @Override
    public void setMasterMute(boolean mute, int flags, String callingPackage, int userId,
            String attributionTag) {
        AudioSystem.setMasterMute(mute);
    }

    @Override
    public boolean isVolumeFixed() {
        return false;
    }

    @Override
    public int getUiSoundsStreamType() {
        return ALIAS[AudioSystem.STREAM_SYSTEM];
    }

    @Override
    public List<AudioVolumeGroup> getAudioVolumeGroups() {
        return new ArrayList<>();
    }

    // --- Ringer mode and audio mode --------------------------------------------------------

    @Override
    public int getRingerModeExternal() {
        synchronized (mLock) {
            return mRingerMode;
        }
    }

    @Override
    public int getRingerModeInternal() {
        return getRingerModeExternal();
    }

    @Override
    public boolean isValidRingerMode(int ringerMode) {
        return ringerMode >= AudioManager.RINGER_MODE_SILENT
                && ringerMode <= AudioManager.RINGER_MODE_NORMAL;
    }

    @Override
    public void setRingerModeExternal(int ringerMode, String caller) {
        if (!isValidRingerMode(ringerMode)) {
            throw new IllegalArgumentException("Bad ringer mode " + ringerMode);
        }
        synchronized (mLock) {
            mRingerMode = ringerMode;
            for (int alias : new int[] {AudioSystem.STREAM_RING, AudioSystem.STREAM_NOTIFICATION}) {
                apply(alias, index(alias));
            }
            Log.i(TAG, "ringer mode " + ringerMode + " (by " + caller + ")");
        }
    }

    @Override
    public void setRingerModeInternal(int ringerMode, String caller) {
        setRingerModeExternal(ringerMode, caller);
    }

    @Override
    public void setMode(int mode, IBinder cb, String callingPackage) {
        synchronized (mLock) {
            mMode = mode;
        }
    }

    @Override
    public int getMode() {
        synchronized (mLock) {
            return mMode;
        }
    }

    @Override
    public boolean isMusicActive(boolean remotely) {
        return !remotely && AudioSystem.isStreamActive(AudioSystem.STREAM_MUSIC, 0);
    }

    // --- Players ----------------------------------------------------------------------------

    @Override
    public int trackPlayer(PlayerBase.PlayerIdCard pic) {
        synchronized (mLock) {
            return mNextPiid++;
        }
    }

    @Override
    public List<AudioPlaybackConfiguration> getActivePlaybackConfigurations() {
        return new ArrayList<>();
    }

    @Override
    public void registerPlaybackCallback(IPlaybackConfigDispatcher pcdb) {
    }

    @Override
    public void playSoundEffect(int effectType, int userId) {
        // Touch sounds are off, as Windows apps have none.
    }

    @Override
    public void playSoundEffectVolume(int effectType, float volume) {
    }

    // --- Audio focus ------------------------------------------------------------------------

    /** A focus holder or waiter, on the stack. */
    private static final class FocusRequester {
        final String clientId;
        final String packageName;
        final IAudioFocusDispatcher dispatcher;
        final IBinder binder;
        int gain;
        /** The loss it was last told of, 0 while it holds focus. */
        int lossReceived;
        IBinder.DeathRecipient death;

        FocusRequester(String clientId, String packageName, IAudioFocusDispatcher dispatcher,
                IBinder binder, int gain) {
            this.clientId = clientId;
            this.packageName = packageName;
            this.dispatcher = dispatcher;
            this.binder = binder;
            this.gain = gain;
        }
    }

    /** Top of the stack (index 0) holds focus. */
    private final ArrayList<FocusRequester> mFocusStack = new ArrayList<>();

    /** The loss a holder takes when another requests `gain` (MediaFocusControl's rules). */
    static int lossFor(int gain) {
        switch (gain) {
            case AudioManager.AUDIOFOCUS_GAIN:
                return AudioManager.AUDIOFOCUS_LOSS;
            case AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK:
                return AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK;
            default:
                return AudioManager.AUDIOFOCUS_LOSS_TRANSIENT;
        }
    }

    private static void dispatch(FocusRequester fr, int change) {
        if (fr.dispatcher == null) return;
        try {
            fr.dispatcher.dispatchAudioFocusChange(change, fr.clientId);
        } catch (RemoteException e) {
            Log.w(TAG, "focus change to " + fr.clientId + " not delivered: " + e);
        }
    }

    @Override
    public int requestAudioFocus(AudioAttributes aa, int focusReqType, IBinder cb,
            IAudioFocusDispatcher fd, String clientId, String callingPackageName,
            String attributionTag, int flags, IAudioPolicyCallback pcb, int sdk) {
        synchronized (mLock) {
            FocusRequester top = mFocusStack.isEmpty() ? null : mFocusStack.get(0);
            if (top != null && top.clientId.equals(clientId)) {
                top.gain = focusReqType;
                return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            }
            removeLocked(clientId);
            int loss = lossFor(focusReqType);
            // The others lose it: a permanent loss takes them off the stack.
            for (int i = mFocusStack.size() - 1; i >= 0; i--) {
                FocusRequester fr = mFocusStack.get(i);
                int worse = fr.lossReceived == 0 ? loss
                        : (loss == AudioManager.AUDIOFOCUS_LOSS ? loss : fr.lossReceived);
                if (worse != fr.lossReceived) {
                    fr.lossReceived = worse;
                    dispatch(fr, worse);
                }
                if (worse == AudioManager.AUDIOFOCUS_LOSS) {
                    unlink(fr);
                    mFocusStack.remove(i);
                }
            }
            FocusRequester fr = new FocusRequester(clientId, callingPackageName, fd, cb,
                    focusReqType);
            if (cb != null) {
                fr.death = () -> {
                    synchronized (mLock) {
                        abandonLocked(clientId);
                    }
                };
                try {
                    cb.linkToDeath(fr.death, 0);
                } catch (RemoteException e) {
                    return AudioManager.AUDIOFOCUS_REQUEST_FAILED;  // already dead
                }
            }
            mFocusStack.add(0, fr);
            Log.i(TAG, "audio focus " + focusReqType + " to " + callingPackageName + " ("
                    + clientId + ")");
            return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        }
    }

    @Override
    public int abandonAudioFocus(IAudioFocusDispatcher fd, String clientId, AudioAttributes aa,
            String callingPackageName) {
        synchronized (mLock) {
            abandonLocked(clientId);
        }
        return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }

    @Override
    public void unregisterAudioFocusClient(String clientId) {
        synchronized (mLock) {
            abandonLocked(clientId);
        }
    }

    private void abandonLocked(String clientId) {
        boolean wasTop = !mFocusStack.isEmpty() && mFocusStack.get(0).clientId.equals(clientId);
        if (!removeLocked(clientId) || !wasTop || mFocusStack.isEmpty()) return;
        // The next one gets focus back.
        FocusRequester next = mFocusStack.get(0);
        if (next.lossReceived != 0) {
            next.lossReceived = 0;
            dispatch(next, next.gain);
            Log.i(TAG, "audio focus back to " + next.packageName + " (" + next.clientId + ")");
        }
    }

    private boolean removeLocked(String clientId) {
        for (int i = 0; i < mFocusStack.size(); i++) {
            FocusRequester fr = mFocusStack.get(i);
            if (fr.clientId.equals(clientId)) {
                unlink(fr);
                mFocusStack.remove(i);
                return true;
            }
        }
        return false;
    }

    private static void unlink(FocusRequester fr) {
        if (fr.binder != null && fr.death != null) {
            try {
                fr.binder.unlinkToDeath(fr.death, 0);
            } catch (Exception e) {
                // Already gone.
            }
        }
    }

    @Override
    public int getCurrentAudioFocus() {
        synchronized (mLock) {
            return mFocusStack.isEmpty() ? AudioManager.AUDIOFOCUS_NONE : mFocusStack.get(0).gain;
        }
    }
}
