/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.util.Log;
import com.gesher.server.WindowsBluetooth;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;

/**
 * BluetoothSocket with the Bluetooth module's public API: RFCOMM over Winsock's AF_BTH
 * sockets ({@link WindowsBluetooth}; docs/bluetooth.md). {@link #connect()} looks the
 * service UUID up in the device's SDP records (Windows does) or uses the channel of
 * createRfcommSocket. Reading after the peer closed the connection returns -1 for apps
 * targeting Android 17 (API 37) and up, and throws IOException for older targets, as on
 * Android; a socket closed locally and native errors always throw IOException.
 *
 * <p>Windows applies its own security: a device must be paired with Windows (Settings,
 * Bluetooth &amp; devices) for most RFCOMM services; "insecure" sockets skip the explicit
 * authentication request.
 */
public final class BluetoothSocket implements Closeable {
    private static final String TAG = "BluetoothSocket";

    public static final int TYPE_RFCOMM = 1;
    public static final int TYPE_SCO = 2;
    public static final int TYPE_L2CAP = 3;
    public static final int TYPE_L2CAP_LE = 4;
    public static final int TYPE_LE = TYPE_L2CAP_LE;
    /** RFCOMM's default frame size, what Android reports for RFCOMM sockets. */
    private static final int RFCOMM_PACKET = 990;
    /** First target SDK (Android 17) whose streams report a clean peer EOF as -1. */
    private static final int PEER_EOF_RETURNS_MINUS_ONE_SDK = 37;

    private final BluetoothDevice mDevice;
    private final UUID mUuid;
    private final boolean mSecure;
    private int mChannel;
    private final Object mLock = new Object();
    private long mNative;
    private boolean mConnected;
    private boolean mConnecting;
    private boolean mSecureCounted;
    private boolean mClosed;
    /** The peer closed the connection cleanly (native read returned 0); with mLock held. */
    private boolean mPeerEof;
    private final InputStream mInput = new Input();
    private final OutputStream mOutput = new Output();

    BluetoothSocket(BluetoothDevice device, UUID uuid, int channel, boolean secure) {
        mDevice = device;
        mUuid = uuid;
        mChannel = channel;
        mSecure = secure;
    }

    /** An accepted connection of a BluetoothServerSocket. */
    BluetoothSocket(BluetoothDevice device, long handle, int channel) {
        this(device, null, channel, true);
        mNative = handle;
        mConnected = true;
    }

    public void connect() throws IOException {
        synchronized (mLock) {
            if (mClosed) throw new IOException("socket closed");
            if (mConnected) throw new IOException("already connected");
            if (mConnecting) throw new IOException("connection already in progress");
            mConnecting = true;
        }
        try {
            connectOnce();
        } finally {
            synchronized (mLock) {
                mConnecting = false;
            }
        }
    }

    private void connectOnce() throws IOException {
        BluetoothHost.enforceConnect(mDevice.context());
        if (!BluetoothHost.on()) throw new IOException("Bluetooth is off");
        int[] hr = new int[1];
        long handle = WindowsBluetooth.nativeRfcommOpen(mDevice.value(),
                mUuid == null ? null : mUuid.toString(), mChannel, mSecure, hr);
        if (handle == 0) throw failed(hr[0]);
        synchronized (mLock) {
            if (mClosed) {
                WindowsBluetooth.nativeRfcommClose(handle);
                throw new IOException("socket closed");
            }
            // close() from another thread now ends the connect below, as on Android.
            mNative = handle;
        }
        int[] channel = new int[] {mChannel};
        int result = WindowsBluetooth.nativeRfcommConnect(handle, channel);
        synchronized (mLock) {
            if (mClosed) throw new IOException("socket closed");
            if (result < 0) {
                mNative = 0;
                WindowsBluetooth.nativeRfcommClose(handle);
                throw failed(result);
            }
            mChannel = channel[0];
            mPeerEof = false;  // EOF belongs to the previous connection only.
            mConnected = true;
            if (mSecure) {
                secureLink(mDevice.getAddress(), 1);
                mSecureCounted = true;
            }
        }
    }

    /** The link is gone (closed or broken); with mLock held. */
    private void dropLink() {
        mConnected = false;
        if (mSecureCounted) {
            mSecureCounted = false;
            secureLink(mDevice.getAddress(), -1);
        }
    }

    private IOException failed(int hr) {
        Log.w(TAG, "connect " + mDevice + " " + (mUuid != null ? mUuid : "channel " + mChannel)
                + ": " + WindowsBluetooth.hresult(hr));
        return new IOException("read failed, socket might closed or timeout, read ret: -1 ("
                + WindowsBluetooth.hresult(hr) + ")");
    }

    // Connected secure sockets per device: Windows opened them with authentication and
    // encryption required (SO_BTH_AUTHENTICATE, SO_BTH_ENCRYPT), so their link is encrypted.
    private static final java.util.Map<String, Integer> sSecureLinks = new java.util.HashMap<>();

    private static void secureLink(String address, int delta) {
        synchronized (sSecureLinks) {
            int n = sSecureLinks.getOrDefault(address, 0) + delta;
            if (n > 0) sSecureLinks.put(address, n); else sSecureLinks.remove(address);
        }
    }

    /** Whether a secure socket of this process is connected to {@code address}. */
    static boolean encryptedLink(String address) {
        synchronized (sSecureLinks) {
            return sSecureLinks.containsKey(address);
        }
    }

    public boolean isConnected() {
        synchronized (mLock) {
            return mConnected && !mClosed;
        }
    }

    @Override
    public void close() throws IOException {
        long handle;
        synchronized (mLock) {
            if (mClosed) return;
            mClosed = true;
            dropLink();
            handle = mNative;
            mNative = 0;
        }
        if (handle != 0) WindowsBluetooth.nativeRfcommClose(handle);
    }

    public InputStream getInputStream() throws IOException {
        return mInput;
    }

    public OutputStream getOutputStream() throws IOException {
        return mOutput;
    }

    public BluetoothDevice getRemoteDevice() {
        return mDevice;
    }

    public int getConnectionType() {
        return TYPE_RFCOMM;
    }

    public int getMaxTransmitPacketSize() {
        return RFCOMM_PACKET;
    }

    public int getMaxReceivePacketSize() {
        return RFCOMM_PACKET;
    }

    /** @hide The RFCOMM channel. */
    public int getPort() {
        return mChannel;
    }

    private long handle() throws IOException {
        synchronized (mLock) {
            if (mClosed || mNative == 0 || !mConnected) throw new IOException("socket closed");
            return mNative;
        }
    }

    /** Whether a clean peer EOF reads as -1 (target Android 17+) instead of an IOException. */
    private boolean peerEofReturnsMinusOne() {
        return BluetoothHost.targetSdk(BluetoothHost.context(mDevice.context()))
                >= PEER_EOF_RETURNS_MINUS_ONE_SDK;
    }

    private final class Input extends InputStream {
        private final byte[] mSingleByte = new byte[1];

        @Override
        public synchronized int read() throws IOException {
            // Serialize only users of this buffer. Socket.close() does not take
            // this monitor and can still interrupt a blocked native read.
            int n = read(mSingleByte, 0, 1);
            return n < 0 ? -1 : mSingleByte[0] & 0xff;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (off < 0 || len < 0 || len > b.length - off) throw new IndexOutOfBoundsException();
            if (len == 0) return 0;
            synchronized (mLock) {
                // Reading again after the peer's EOF keeps returning -1 (InputStream contract).
                if (mPeerEof && !mClosed && peerEofReturnsMinusOne()) return -1;
            }
            long reading = handle();
            int n = WindowsBluetooth.nativeRfcommRead(reading, b, off, len);
            if (n > 0) return n;
            boolean eof;
            synchronized (mLock) {
                // Native 0 is the end of the stream, and also what a read woken by a local
                // close() reports; only the former is a peer EOF.
                eof = n == 0 && !mClosed;
                // A second read from the old connection can finish after a
                // successful reconnect. Its EOF must not poison the new link.
                if (mNative == reading) {
                    if (eof) mPeerEof = true;
                    dropLink();
                }
            }
            if (eof && peerEofReturnsMinusOne()) return -1;
            throw new IOException("bt socket closed, read return: " + (n == 0 ? -1 : n));
        }

        @Override
        public int available() throws IOException {
            return WindowsBluetooth.nativeRfcommAvailable(handle());
        }

        @Override
        public void close() throws IOException {
            BluetoothSocket.this.close();
        }
    }

    private final class Output extends OutputStream {
        private final byte[] mSingleByte = new byte[1];

        @Override
        public synchronized void write(int b) throws IOException {
            mSingleByte[0] = (byte) b;
            write(mSingleByte, 0, 1);
        }

        @Override
        public void write(byte[] b, int off, int len) throws IOException {
            if (off < 0 || len < 0 || len > b.length - off) throw new IndexOutOfBoundsException();
            if (len == 0) return;
            int n = WindowsBluetooth.nativeRfcommWrite(handle(), b, off, len);
            if (n < 0) {
                synchronized (mLock) {
                    dropLink();
                }
                throw new IOException("bt socket closed, write return: " + n);
            }
        }

        @Override
        public void close() throws IOException {
            BluetoothSocket.this.close();
        }
    }

    @Override
    public String toString() {
        return "BluetoothSocket{" + mDevice + ", RFCOMM channel " + mChannel + "}";
    }
}
