package com.gesher.app.window;

import android.app.WindowConfiguration;
import android.content.res.Configuration;
import android.graphics.Rect;
import com.gesher.app.DeviceProfile;
import org.junit.Test;
import static org.junit.Assert.*;

public class MultiWindowContractTest {

    @Test
    public void testWindowingModeIsFreeform() {
        Configuration config = DeviceProfile.configuration();
        int mode = config.windowConfiguration.getWindowingMode();
        assertEquals("Windowing mode should be FREEFORM", WindowConfiguration.WINDOWING_MODE_FREEFORM, mode);
    }

    @Test
    public void testIsInMultiWindowModeReturnsTrue() {
        WindowSession session = new WindowSession();
        assertTrue("isInMultiWindowMode should return true for desktop windows", session.isInMultiWindowMode());
    }

    @Test
    public void testWindowBoundaries() {
        Configuration config = DeviceProfile.configuration();
        Rect bounds = config.windowConfiguration.getBounds();
        Rect appBounds = config.windowConfiguration.getAppBounds();
        Rect maxBounds = config.windowConfiguration.getMaxBounds();
        
        Rect expected = new Rect(0, 0, DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        
        assertEquals(expected, bounds);
        assertEquals(expected, appBounds);
        assertEquals(expected, maxBounds);
    }
}
