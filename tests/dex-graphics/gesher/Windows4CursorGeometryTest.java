package gesher;

import android.graphics.Matrix;
import android.view.inputmethod.CursorAnchorInfo;
import com.gesher.app.input.CursorAnchorGeometry;
import java.util.Arrays;

/** Actual CursorAnchorInfo/Matrix coordinates, independent of insertion-marker availability. */
public final class Windows4CursorGeometryTest {
    private static void equal(int[] expected, int[] actual, String message) {
        if (!Arrays.equals(expected, actual)) {
            throw new AssertionError(message + ": " + Arrays.toString(actual));
        }
        System.out.println("PASS windows4 cursor geometry: " + message);
    }

    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        Matrix matrix = new Matrix();
        matrix.setScale(2, 2);
        matrix.postTranslate(100, 200);
        CursorAnchorInfo measured = new CursorAnchorInfo.Builder()
                .setMatrix(matrix).setSelectionRange(7, 11).setComposingText(7, "A\ud83d\ude00B")
                .addCharacterBounds(7, 1.25f, 2.5f, 4.25f, 6.75f, CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION)
                .addCharacterBounds(8, 4.25f, 2.5f, 7.5f, 6.75f, CursorAnchorInfo.FLAG_HAS_INVISIBLE_REGION)
                .addCharacterBounds(9, 4.25f, 2.5f, 7.5f, 6.75f, CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION)
                .build();
        if (!Float.isNaN(measured.getInsertionMarkerHorizontal())) {
            throw new AssertionError("Fixture unexpectedly supplies an insertion marker");
        }
        equal(new int[] {7,22,55,29,64,0, 8,28,55,35,64,1, 9,28,55,35,64,0, -1,0,0,0,0,0},
                CursorAnchorGeometry.characters(measured, 80, 150),
                "missing caret retains measured UTF-16 character bounds, transform, clipping and holes");
        CursorAnchorInfo missing = new CursorAnchorInfo.Builder()
                .setMatrix(matrix).setSelectionRange(7, 11).setComposingText(7, "A\ud83d\ude00B").build();
        equal(new int[] {-1,0,0,0,0,0, -1,0,0,0,0,0, -1,0,0,0,0,0, -1,0,0,0,0,0},
                CursorAnchorGeometry.characters(missing, 80, 150),
                "new same-text observation explicitly marks every missing measurement");
        equal(new int[0], CursorAnchorGeometry.characters(new CursorAnchorInfo.Builder().build(), 0, 0),
                "ended composition clears measured text geometry");
        System.out.println("Windows4CursorGeometryTest all passed");
    }
}
