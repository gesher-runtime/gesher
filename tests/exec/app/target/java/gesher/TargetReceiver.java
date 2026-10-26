package gesher;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Process;

public final class TargetReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        System.out.println("Exec3Target BROADCAST action=" + intent.getAction() + " payload="
                + intent.getStringExtra("payload") + " pid=" + Process.myPid());
        System.out.flush();
    }
}
