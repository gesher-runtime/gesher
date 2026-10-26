// The shell through ART's actual ProcessBuilder pipes, including an open stdin.
package gesher;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

public final class Exec3Test {
    private static String line(BufferedReader input) throws Exception {
        FutureTask<String> read = new FutureTask<>(input::readLine);
        Thread thread = new Thread(read, "shell-output");
        thread.setDaemon(true);
        thread.start();
        return read.get(20, TimeUnit.SECONDS);
    }

    private static void send(OutputStream output, String text) throws Exception {
        output.write(text.getBytes(StandardCharsets.UTF_8));
        output.flush();
    }

    private static void expect(BufferedReader input, String wanted) throws Exception {
        String actual = line(input);
        if (!wanted.equals(actual)) throw new AssertionError("expected <" + wanted + ">, got <" + actual + ">");
    }

    public static void main(String[] args) throws Exception {
        String shell = args.length == 0 ? "/system/bin/sh" : args[0];
        Process process = new ProcessBuilder(shell).redirectErrorStream(true).start();
        try {
            BufferedReader input = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
            OutputStream output = process.getOutputStream();
            send(output, "echo first\n");
            expect(input, "first");
            send(output, "read -r value\nhello world\necho \"$value\"\n");
            expect(input, "hello world");
            send(output, "for i in 1 2; do\necho \"item:$i\"\ndone\n");
            expect(input, "item:1");
            expect(input, "item:2");
            send(output, "printf 'piped\\n' | cat\necho \"$(echo substituted)\"\n");
            expect(input, "piped");
            expect(input, "substituted");
            send(output, "trap 'echo exit:$?' EXIT\nexit 7\n");
            output.close();
            expect(input, "exit:7");
            if (!process.waitFor(20, TimeUnit.SECONDS) || process.exitValue() != 7) {
                throw new AssertionError("shell did not return status 7");
            }
            System.out.println("Exec3Test PASS incremental stdin, read, multiline loop, pipeline, substitution, EXIT trap and status");
        } finally {
            process.destroyForcibly();
        }
    }
}
