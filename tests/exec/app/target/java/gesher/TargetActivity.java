package gesher;
import android.app.Activity;
import android.os.Bundle;
import android.os.Process;
import android.widget.TextView;

public final class TargetActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Exec3 am command target");
        setContentView(text);
    }
    @Override public void onResume() {
        super.onResume();
        System.out.println("Exec3Target READY pid=" + Process.myPid());
        System.out.flush();
    }
}
