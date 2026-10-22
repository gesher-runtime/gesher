package gesher;

import java.nio.ByteBuffer;
import java.nio.channels.Pipe;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.spi.SelectorProvider;

/**
 * libopenjdk's EPoll natives over gesher-base's epoll: an EPollSelector
 * (taken by name; Android's default is the poll one) sees a pipe become
 * readable.
 */
public final class EPollTest {
  public static void main(String[] args) throws Exception {
    SelectorProvider provider = (SelectorProvider)
        Class.forName("sun.nio.ch.EPollSelectorProvider").getDeclaredConstructor().newInstance();
    Pipe pipe = provider.openPipe();
    pipe.source().configureBlocking(false);
    try (Selector selector = provider.openSelector()) {
      pipe.source().register(selector, SelectionKey.OP_READ);
      check(selector.selectNow() == 0, "nothing ready before the write");
      pipe.sink().write(ByteBuffer.wrap(new byte[] {42}));
      check(selector.select(5000) == 1, "pipe readable after the write");
      ByteBuffer in = ByteBuffer.allocate(1);
      check(pipe.source().read(in) == 1 && in.get(0) == 42, "the byte read back");
    }
    System.out.println("EPollTest all passed");
  }

  private static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
  }
}
