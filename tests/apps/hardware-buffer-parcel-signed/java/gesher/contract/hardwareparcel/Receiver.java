package gesher.contract.hardwareparcel;

import android.app.Service;
import android.content.Intent;
import android.graphics.Bitmap;
import android.hardware.HardwareBuffer;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;

public final class Receiver extends Service {
    private final Binder endpoint = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code != ProbeActivity.REQUEST) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(ProbeActivity.DESCRIPTOR);
            ProbeActivity.require(data.readInt() == 42, "prefix field");
            try (HardwareBuffer buffer = HardwareBuffer.CREATOR.createFromParcel(data)) {
                ProbeActivity.require(buffer != null, "received buffer");
                ProbeActivity.require(data.readInt() == 99 && data.dataAvail() == 0, "trailer field");
                ProbeActivity.require(buffer.getWidth() == 18 && buffer.getHeight() == 10
                        && buffer.getFormat() == HardwareBuffer.RGBA_8888 && buffer.getLayers() == 1,
                        "received dimensions and format");
                Bitmap gpu = Bitmap.wrapHardwareBuffer(buffer, null);
                ProbeActivity.require(gpu != null, "received pixels through hardware buffer");
                try {
                    Bitmap cpu = gpu.copy(Bitmap.Config.ARGB_8888, false);
                    ProbeActivity.require(cpu != null, "received pixels can be read");
                    try {
                        reply.writeNoException();
                        reply.writeInt(Process.myPid());
                        reply.writeLong(buffer.getId());
                        reply.writeInt(cpu.getPixel(2, 6));
                        reply.writeInt(cpu.getPixel(15, 6));
                    } finally { cpu.recycle(); }
                } finally { gpu.recycle(); }
            }
            return true;
        }
    };

    @Override public IBinder onBind(Intent intent) {
        return endpoint;
    }
}
