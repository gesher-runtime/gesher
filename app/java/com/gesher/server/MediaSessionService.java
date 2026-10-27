/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ParceledListSlice;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.AudioSystem;
import android.media.MediaMetadata;
import android.media.Rating;
import android.media.VolumeProvider;
import android.media.session.IActiveSessionsListener;
import android.media.session.IOnMediaKeyEventDispatchedListener;
import android.media.session.IOnMediaKeyEventSessionChangedListener;
import android.media.session.ISession;
import android.media.session.ISessionCallback;
import android.media.session.ISessionController;
import android.media.session.ISessionControllerCallback;
import android.media.session.ISessionManager;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.ParcelableListBinder;
import android.media.session.PlaybackState;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.os.UserHandle;
import android.view.KeyEvent;
import java.util.ArrayList;
import java.util.List;

/**
 * media_session: sessions owned by app processes, with Binder controllers and callbacks.
 * Metadata, playback state and queues live here; transport commands go back to the
 * owning app's callback. Owner death releases its sessions (docs/media.md).
 */
public final class MediaSessionService extends ISessionManager.Default {
    private final AudioService mAudio;
    private final Object mLock = new Object();
    private final List<Session> mSessions = new ArrayList<>();
    private final Callbacks<IActiveSessionsListener> mListeners = new Callbacks<>();
    private final Callbacks<IOnMediaKeyEventSessionChangedListener> mKeySessions =
            new Callbacks<>();
    private final Callbacks<IOnMediaKeyEventDispatchedListener> mKeyEvents = new Callbacks<>();

    public MediaSessionService(AudioService audio) {
        mAudio = audio;
    }

    private interface RemoteCall<T> {
        void run(T target) throws RemoteException;
    }

    /** Snapshot under the list lock, then call remote code without any service lock. */
    private static final class Callbacks<T extends IInterface> {
        private final RemoteCallbackList<T> mList = new RemoteCallbackList<>();
        synchronized void add(T callback) {
            if (callback != null)
                mList.register(callback);
        }
        synchronized void remove(T callback) {
            if (callback != null)
                mList.unregister(callback);
        }
        void send(RemoteCall<T> call) {
            List<T> targets = new ArrayList<>();
            synchronized (this) {
                int n = mList.beginBroadcast();
                for (int i = 0; i < n; i++) targets.add(mList.getBroadcastItem(i));
                mList.finishBroadcast();
            }
            for (T target : targets) {
                try {
                    call.run(target);
                } catch (RemoteException e) {
                    remove(target);
                }
            }
        }
    }

    @Override
    public ISession createSession(String pkg, ISessionCallback callback, String tag, Bundle info,
            int userId) throws RemoteException {
        if (pkg == null || callback == null || tag == null) {
            throw new IllegalArgumentException("A session needs a package, callback and tag");
        }
        Session s = new Session(pkg, callback, tag, info, userId, Binder.getCallingUid());
        synchronized (mLock) {
            mSessions.add(s);
        }
        try {
            callback.asBinder().linkToDeath(s, 0);
        } catch (RemoteException e) {
            s.destroySession();
            throw e;
        }
        return s.owner;
    }

    @Override
    public List<MediaSession.Token> getSessions(ComponentName listener, int userId) {
        List<MediaSession.Token> result = new ArrayList<>();
        synchronized (mLock) {
            for (int i = mSessions.size() - 1; i >= 0; i--) {
                Session s = mSessions.get(i);
                if (s.active && !s.destroyed
                        && (userId == UserHandle.USER_ALL || userId == s.userId)) {
                    result.add(s.token);
                }
            }
        }
        return result;
    }

    private Session selected() {
        synchronized (mLock) {
            for (int i = mSessions.size() - 1; i >= 0; i--) {
                Session s = mSessions.get(i);
                if (s.active && !s.destroyed)
                    return s;
            }
        }
        return null;
    }

    private Session find(MediaSession.Token token) {
        synchronized (mLock) {
            for (Session s : mSessions)
                if (!s.destroyed && s.token.equals(token))
                    return s;
        }
        return null;
    }

    private void changed() {
        List<MediaSession.Token> tokens = getSessions(null, UserHandle.USER_ALL);
        mListeners.send(c -> c.onActiveSessionsChanged(tokens));
        Session s = selected();
        mKeySessions.send(c
                -> c.onMediaKeyEventSessionChanged(
                        s != null ? s.pkg : "", s != null ? s.token : null));
    }

    @Override
    public MediaSession.Token getMediaKeyEventSession(String pkg) {
        Session s = selected();
        return s != null ? s.token : null;
    }
    @Override
    public String getMediaKeyEventSessionPackageName(String pkg) {
        Session s = selected();
        return s != null ? s.pkg : "";
    }
    @Override
    public void addSessionsListener(
            IActiveSessionsListener listener, ComponentName component, int userId) {
        mListeners.add(listener);
    }
    @Override
    public void removeSessionsListener(IActiveSessionsListener listener) {
        mListeners.remove(listener);
    }
    @Override
    public void addOnMediaKeyEventSessionChangedListener(
            IOnMediaKeyEventSessionChangedListener listener, String pkg) {
        mKeySessions.add(listener);
        Session s = selected();
        try {
            listener.onMediaKeyEventSessionChanged(
                    s != null ? s.pkg : "", s != null ? s.token : null);
        } catch (RemoteException e) {
            mKeySessions.remove(listener);
        }
    }
    @Override
    public void removeOnMediaKeyEventSessionChangedListener(
            IOnMediaKeyEventSessionChangedListener listener) {
        mKeySessions.remove(listener);
    }
    @Override
    public void addOnMediaKeyEventDispatchedListener(IOnMediaKeyEventDispatchedListener listener) {
        mKeyEvents.add(listener);
    }
    @Override
    public void removeOnMediaKeyEventDispatchedListener(
            IOnMediaKeyEventDispatchedListener listener) {
        mKeyEvents.remove(listener);
    }
    @Override
    public boolean isGlobalPriorityActive() {
        return false;
    }

    private boolean mediaButton(Session s, String pkg, KeyEvent event) throws RemoteException {
        if (s == null || event == null || !KeyEvent.isMediaSessionKey(event.getKeyCode()))
            return false;
        s.callback.onMediaButtonFromController(pkg, Binder.getCallingPid(), Binder.getCallingUid(),
                new Intent(Intent.ACTION_MEDIA_BUTTON).putExtra(Intent.EXTRA_KEY_EVENT, event));
        mKeyEvents.send(c -> c.onMediaKeyEventDispatched(event, s.pkg, s.token));
        return true;
    }
    @Override
    public void dispatchMediaKeyEvent(String pkg, boolean system, KeyEvent event, boolean wakeLock)
            throws RemoteException {
        mediaButton(selected(), pkg, event);
    }
    @Override
    public boolean dispatchMediaKeyEventToSessionAsSystemService(
            String pkg, KeyEvent event, MediaSession.Token token) throws RemoteException {
        return mediaButton(find(token), pkg, event);
    }

    private void volumeKey(Session session, String pkg, String opPkg, KeyEvent event, int stream)
            throws RemoteException {
        if (event == null || event.getAction() != KeyEvent.ACTION_DOWN)
            return;
        int direction;
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_VOLUME_UP:
                direction = AudioManager.ADJUST_RAISE;
                break;
            case KeyEvent.KEYCODE_VOLUME_DOWN:
                direction = AudioManager.ADJUST_LOWER;
                break;
            case KeyEvent.KEYCODE_VOLUME_MUTE:
                direction = AudioManager.ADJUST_TOGGLE_MUTE;
                break;
            default:
                return;
        }
        if (session != null)
            session.controller.adjustVolume(pkg, opPkg, direction, 0);
        else
            mAudio.adjustSuggestedStreamVolume(direction, stream, 0);
    }

    @Override
    public void dispatchVolumeKeyEvent(String pkg, String opPkg, boolean system, KeyEvent event,
            int stream, boolean musicOnly) throws RemoteException {
        volumeKey(selected(), pkg, opPkg, event, stream);
    }

    @Override
    public void dispatchVolumeKeyEventToSessionAsSystemService(String pkg, String opPkg,
            KeyEvent event, MediaSession.Token token) throws RemoteException {
        volumeKey(find(token), pkg, opPkg, event, AudioSystem.STREAM_MUSIC);
    }

    @Override
    public void dispatchAdjustVolume(String pkg, String opPkg, int stream, int delta, int flags)
            throws RemoteException {
        Session s = selected();
        if (s != null)
            s.controller.adjustVolume(pkg, opPkg, delta, flags);
        else
            mAudio.adjustSuggestedStreamVolume(delta, stream, flags);
    }

    public final class Session extends ISession.Default implements IBinder.DeathRecipient {
        final String pkg, tag;
        final int userId;
        final Bundle info;
        final ISessionCallback callback;
        final ISession owner;
        final ISessionController controller;
        final MediaSession.Token token;
        final Callbacks<ISessionControllerCallback> observers = new Callbacks<>();
        boolean active, destroyed;
        int flags, ratingType, volumeType = MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL;
        int volumeControl = VolumeProvider.VOLUME_CONTROL_ABSOLUTE, maxVolume = 15, volume = 15;
        String volumeId;
        AudioAttributes attributes =
                new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
        PendingIntent launch;
        MediaMetadata metadata;
        PlaybackState playback;
        List<MediaSession.QueueItem> queue;
        CharSequence queueTitle;
        Bundle extras;

        Session(String pkg, ISessionCallback callback, String tag, Bundle info, int userId,
                int uid) {
            this.pkg = pkg;
            this.callback = callback;
            this.tag = tag;
            this.info = copy(info);
            this.userId = userId;
            owner = StandIn.of(ISession.class, this).asInterface();
            controller = StandIn.of(ISessionController.class, new Controller()).asInterface();
            token = new MediaSession.Token(uid, controller);
        }

        @Override
        public void binderDied() {
            destroySession();
        }
        @Override
        public ISessionController getController() {
            return controller;
        }
        @Override
        public void setFlags(int value) {
            synchronized (mLock) {
                flags = value;
            }
        }
        @Override
        public void setActive(boolean value) {
            synchronized (mLock) {
                if (destroyed)
                    return;
                active = value;
            }
            changed();
        }
        @Override
        public void destroySession() {
            synchronized (mLock) {
                if (destroyed)
                    return;
                destroyed = true;
                active = false;
                mSessions.remove(this);
            }
            try {
                callback.asBinder().unlinkToDeath(this, 0);
            } catch (java.util.NoSuchElementException
                            e) { /* Owner died before linkToDeath completed. */
            }
            observers.send(ISessionControllerCallback::onSessionDestroyed);
            changed();
        }
        @Override
        public void sendEvent(String name, Bundle data) {
            observers.send(c -> c.onEvent(name, copy(data)));
        }
        @Override
        public void setLaunchPendingIntent(PendingIntent value) {
            synchronized (mLock) {
                launch = value;
            }
        }
        @Override
        public void setMetadata(MediaMetadata value, long duration, String description) {
            synchronized (mLock) {
                metadata = value;
            }
            observers.send(c -> c.onMetadataChanged(value));
        }
        @Override
        public void setPlaybackState(PlaybackState value) {
            synchronized (mLock) {
                playback = value;
            }
            observers.send(c -> c.onPlaybackStateChanged(value));
        }
        private void queue(List<MediaSession.QueueItem> value) {
            synchronized (mLock) {
                queue = value != null ? new ArrayList<>(value) : null;
            }
            observers.send(
                    c -> c.onQueueChanged(value != null ? new ParceledListSlice<>(value) : null));
        }
        @Override
        public void resetQueue() {
            queue(null);
        }
        @Override
        public IBinder getBinderForSetQueue() {
            return new ParcelableListBinder<>(MediaSession.QueueItem.class, this::queue);
        }
        @Override
        public void setQueueTitle(CharSequence value) {
            synchronized (mLock) {
                queueTitle = value;
            }
            observers.send(c -> c.onQueueTitleChanged(value));
        }
        @Override
        public void setExtras(Bundle value) {
            synchronized (mLock) {
                extras = copy(value);
            }
            observers.send(c -> c.onExtrasChanged(copy(value)));
        }
        @Override
        public void setRatingType(int value) {
            synchronized (mLock) {
                ratingType = value;
            }
        }
        private MediaController.PlaybackInfo volumeInfo() {
            synchronized (mLock) {
                if (volumeType == MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL) {
                    int stream = localStream();
                    return new MediaController.PlaybackInfo(volumeType,
                            VolumeProvider.VOLUME_CONTROL_ABSOLUTE,
                            mAudio.getStreamMaxVolume(stream), mAudio.getStreamVolume(stream),
                            attributes, null);
                }
                return new MediaController.PlaybackInfo(
                        volumeType, volumeControl, maxVolume, volume, attributes, volumeId);
            }
        }

        private int localStream() {
            int stream = AudioAttributes.toLegacyStreamType(attributes);
            return stream >= 0 && stream < AudioService.MAX.length ? stream
                                                                   : AudioSystem.STREAM_MUSIC;
        }
        @Override
        public void setPlaybackToLocal(AudioAttributes value) {
            synchronized (mLock) {
                attributes = value;
                volumeType = MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL;
            }
            observers.send(c -> c.onVolumeInfoChanged(volumeInfo()));
        }
        @Override
        public void setPlaybackToRemote(int control, int max, String id) {
            synchronized (mLock) {
                volumeType = MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE;
                volumeControl = control;
                maxVolume = max;
                volumeId = id;
            }
            observers.send(c -> c.onVolumeInfoChanged(volumeInfo()));
        }
        @Override
        public void setCurrentVolume(int value) {
            synchronized (mLock) {
                volume = Math.max(0, Math.min(value, maxVolume));
            }
            observers.send(c -> c.onVolumeInfoChanged(volumeInfo()));
        }

        public final class Controller extends ISessionController.Default {
            private void call(RemoteCall<ISessionCallback> action) throws RemoteException {
                synchronized (mLock) {
                    if (destroyed)
                        return;
                }
                action.run(callback);
            }
            @Override
            public void registerCallback(String pkg, ISessionControllerCallback cb)
                    throws RemoteException {
                boolean ended;
                synchronized (mLock) {
                    ended = destroyed;
                    if (!ended)
                        observers.add(cb);
                }
                if (ended)
                    cb.onSessionDestroyed();
            }
            @Override
            public void unregisterCallback(ISessionControllerCallback cb) {
                observers.remove(cb);
            }
            @Override
            public String getPackageName() {
                return pkg;
            }
            @Override
            public String getTag() {
                return tag;
            }
            @Override
            public Bundle getSessionInfo() {
                return copy(info);
            }
            @Override
            public PendingIntent getLaunchPendingIntent() {
                synchronized (mLock) {
                    return launch;
                }
            }
            @Override
            public long getFlags() {
                synchronized (mLock) {
                    return flags;
                }
            }
            @Override
            public MediaMetadata getMetadata() {
                synchronized (mLock) {
                    return metadata;
                }
            }
            @Override
            public PlaybackState getPlaybackState() {
                synchronized (mLock) {
                    return playback;
                }
            }
            @Override
            public ParceledListSlice getQueue() {
                synchronized (mLock) {
                    return queue != null ? new ParceledListSlice<>(queue) : null;
                }
            }
            @Override
            public CharSequence getQueueTitle() {
                synchronized (mLock) {
                    return queueTitle;
                }
            }
            @Override
            public Bundle getExtras() {
                synchronized (mLock) {
                    return copy(extras);
                }
            }
            @Override
            public int getRatingType() {
                synchronized (mLock) {
                    return ratingType;
                }
            }
            @Override
            public MediaController.PlaybackInfo getVolumeAttributes() {
                return volumeInfo();
            }
            @Override
            public boolean sendMediaButton(String pkg, KeyEvent event) throws RemoteException {
                return mediaButton(Session.this, pkg, event);
            }
            @Override
            public void sendCommand(String pkg, String command, Bundle args, ResultReceiver result)
                    throws RemoteException {
                call(c
                        -> c.onCommand(pkg, Binder.getCallingPid(), Binder.getCallingUid(), command,
                                args, result));
            }
            @Override
            public void prepare(String pkg) throws RemoteException {
                call(c -> c.onPrepare(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void prepareFromMediaId(String pkg, String id, Bundle args)
                    throws RemoteException {
                call(c
                        -> c.onPrepareFromMediaId(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), id, args));
            }
            @Override
            public void prepareFromSearch(String pkg, String query, Bundle args)
                    throws RemoteException {
                call(c
                        -> c.onPrepareFromSearch(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), query, args));
            }
            @Override
            public void prepareFromUri(String pkg, Uri uri, Bundle args) throws RemoteException {
                call(c
                        -> c.onPrepareFromUri(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), uri, args));
            }
            @Override
            public void play(String pkg) throws RemoteException {
                call(c -> c.onPlay(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void playFromMediaId(String pkg, String id, Bundle args) throws RemoteException {
                call(c
                        -> c.onPlayFromMediaId(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), id, args));
            }
            @Override
            public void playFromSearch(String pkg, String query, Bundle args)
                    throws RemoteException {
                call(c
                        -> c.onPlayFromSearch(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), query, args));
            }
            @Override
            public void playFromUri(String pkg, Uri uri, Bundle args) throws RemoteException {
                call(c
                        -> c.onPlayFromUri(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), uri, args));
            }
            @Override
            public void skipToQueueItem(String pkg, long id) throws RemoteException {
                call(c -> c.onSkipToTrack(pkg, Binder.getCallingPid(), Binder.getCallingUid(), id));
            }
            @Override
            public void pause(String pkg) throws RemoteException {
                call(c -> c.onPause(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void stop(String pkg) throws RemoteException {
                call(c -> c.onStop(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void next(String pkg) throws RemoteException {
                call(c -> c.onNext(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void previous(String pkg) throws RemoteException {
                call(c -> c.onPrevious(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void fastForward(String pkg) throws RemoteException {
                call(c -> c.onFastForward(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void rewind(String pkg) throws RemoteException {
                call(c -> c.onRewind(pkg, Binder.getCallingPid(), Binder.getCallingUid()));
            }
            @Override
            public void seekTo(String pkg, long pos) throws RemoteException {
                call(c -> c.onSeekTo(pkg, Binder.getCallingPid(), Binder.getCallingUid(), pos));
            }
            @Override
            public void rate(String pkg, Rating rating) throws RemoteException {
                call(c -> c.onRate(pkg, Binder.getCallingPid(), Binder.getCallingUid(), rating));
            }
            @Override
            public void setPlaybackSpeed(String pkg, float speed) throws RemoteException {
                call(c
                        -> c.onSetPlaybackSpeed(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), speed));
            }
            @Override
            public void sendCustomAction(String pkg, String action, Bundle args)
                    throws RemoteException {
                call(c
                        -> c.onCustomAction(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), action, args));
            }
            @Override
            public void adjustVolume(String pkg, String opPkg, int direction, int flags)
                    throws RemoteException {
                int stream;
                synchronized (mLock) {
                    stream = volumeType == MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL
                            ? localStream()
                            : -1;
                }
                if (stream >= 0) {
                    mAudio.adjustStreamVolume(stream, direction, flags, opPkg);
                    observers.send(c -> c.onVolumeInfoChanged(volumeInfo()));
                    return;
                }
                call(c
                        -> c.onAdjustVolume(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), direction));
            }
            @Override
            public void setVolumeTo(String pkg, String opPkg, int value, int flags)
                    throws RemoteException {
                int stream;
                synchronized (mLock) {
                    stream = volumeType == MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL
                            ? localStream()
                            : -1;
                }
                if (stream >= 0) {
                    mAudio.setStreamVolume(stream, value, flags, opPkg);
                    observers.send(c -> c.onVolumeInfoChanged(volumeInfo()));
                    return;
                }
                call(c
                        -> c.onSetVolumeTo(
                                pkg, Binder.getCallingPid(), Binder.getCallingUid(), value));
            }
        }
    }

    private static Bundle copy(Bundle value) {
        return value != null ? new Bundle(value) : null;
    }
}
