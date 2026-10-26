package gesher;
import com.gesher.app.LogLines;
import java.io.ByteArrayOutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;

public final class LauncherTailTest {
    private static byte[] bytes(String text) { return text.getBytes(StandardCharsets.UTF_8); }
    private static String text(ByteArrayOutputStream output) {
        return new String(output.toByteArray(), StandardCharsets.UTF_8).replace("\r\n", "\n");
    }
    private static void equal(String expected, String actual) {
        if (!expected.equals(actual)) throw new AssertionError("expected <" + expected + ">, got <" + actual + ">");
    }
    public static void main(String[] args) throws Exception {
        ByteArrayOutputStream captured = new ByteArrayOutputStream();
        PrintStream output = new PrintStream(captured, true, "UTF-8");
        LogLines lines = new LogLines(output);
        String frame = "gesher-app: window \"Gesher שלום windows3 main\" 400x800: first frame drawn (1 presented), shown, hwnd 0x1f0218\n";
        byte[] title = bytes(frame);
        int split = bytes("gesher-app: window \"Gesher ").length + 1; // inside a UTF-8 character
        lines.write(title, 0, split);
        equal("", text(captured));
        output.println("gesher-app: startActivityAndWait: result 0");
        lines.write(title, split, title.length - split);
        equal("gesher-app: startActivityAndWait: result 0\n" + frame, text(captured));
        captured.reset();
        byte[] chunk = bytes("one\ntwo\nunterminated שלום");
        lines.write(chunk, 0, chunk.length);
        equal("one\ntwo\n", text(captured));
        output.println("parent-before-finish");
        lines.finish();
        lines.finish();
        output.println("parent-after-finish");
        equal("one\ntwo\nparent-before-finish\nunterminated שלום\nparent-after-finish\n", text(captured));
        captured.reset();
        lines.write(bytes("discarded partial title"), 0, bytes("discarded partial title").length);
        lines.reset();
        lines.write(bytes("new log\n"), 0, bytes("new log\n").length);
        equal("new log\n", text(captured));
        captured.reset();
        Thread parent = new Thread(() -> { for (int i = 0; i < 400; ++i) output.println("parent-" + i); });
        parent.start();
        for (int i = 0; i < 400; ++i) {
            byte[] child = bytes("child-" + i + "-שלום\n");
            for (int at = 0; at < child.length; ++at) lines.write(child, at, 1);
        }
        parent.join();
        String[] all = text(captured).split("\n");
        if (all.length != 800) throw new AssertionError("lost or duplicated concurrent lines");
        for (String line : all) if (!line.matches("parent-\\d+|child-\\d+-שלום")) throw new AssertionError("spliced line: " + line);
        System.out.println("LauncherTailTest PASS partial UTF-8 title, atomic concurrent lines, truncation reset and final fragment");
    }
}
