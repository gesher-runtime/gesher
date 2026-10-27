package gesher;

import android.content.res.Configuration;
import android.os.LocaleList;
import android.view.Surface;
import android.view.View;
import com.gesher.app.DeviceProfile;
import com.gesher.app.window.TaskConfiguration;
import com.gesher.app.window.ApplicationDisplay;

/** Actual Configuration.updateFrom must retain app locale direction for every task rotation. */
public final class Windows4ConfigurationTest {
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        for (String language : new String[] {"he-IL", "ar-EG", "en-US"}) {
            for (int rotation : new int[] {Surface.ROTATION_0, Surface.ROTATION_90,
                    Surface.ROTATION_180, Surface.ROTATION_270}) {
                Configuration global = DeviceProfile.configuration();
                global.setLocales(LocaleList.forLanguageTags(language));
                int direction = global.getLayoutDirection();
                ApplicationDisplay.update(rotation);
                Configuration application = new Configuration(global);
                ApplicationDisplay.applyConfiguration(application);
                if (!application.windowConfiguration.getMaxBounds().equals(TaskConfiguration.display(rotation))
                        || application.windowConfiguration.getDisplayRotation() != rotation
                        || application.getLayoutDirection() != direction
                        || !application.getLocales().equals(global.getLocales())) {
                    throw new AssertionError("Application display configuration: " + application);
                }
                Configuration task = TaskConfiguration.override(TaskConfiguration.display(rotation), rotation);
                if ((task.screenLayout & Configuration.SCREENLAYOUT_LAYOUTDIR_MASK) != 0) {
                    throw new AssertionError("Task geometry defines a locale direction");
                }
                global.updateFrom(task);
                int expected = language.equals("en-US") ? View.LAYOUT_DIRECTION_LTR : View.LAYOUT_DIRECTION_RTL;
                if (direction != expected || global.getLayoutDirection() != expected
                        || !global.getLocales().get(0).toLanguageTag().equals(language)
                        || global.windowConfiguration.getDisplayRotation() != rotation) {
                    throw new AssertionError(language + " rotation " + rotation + ": " + global);
                }
                System.out.println("PASS windows4 Configuration: " + language + " rotation "
                        + rotation + " retains layout direction " + expected);
            }
        }
        System.out.println("Windows4ConfigurationTest all passed");
    }
}
