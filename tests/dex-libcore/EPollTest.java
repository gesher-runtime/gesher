package gesher;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.channels.Pipe;

/**
 * libopenjdk's EPoll natives (sun.nio.ch.EPoll, which EPollPort uses) over
 * gesher-base's epoll: an epoll set sees a pipe become readable.
 */
public final class EPollTest {
  private static final int EPOLLIN = 1;

  public static void main(String[] args) throws Exception {
    Class<?> epoll = Class.forName("sun.nio.ch.EPoll");
    Method create = method(epoll, "epollCreate");
    Method ctl = method(epoll, "epollCtl", int.class, int.class, int.class, int.class);
    Method wait = method(epoll, "epollWait", int.class, long.class, int.class);
    Method allocate = method(epoll, "allocatePollArray", int.class);
    Method getDescriptor = method(epoll, "getDescriptor", long.class);
    Method getEvents = method(epoll, "getEvents", long.class);

    Pipe pipe = Pipe.open();
    Method fdVal = method(Class.forName("sun.nio.ch.SelChImpl"), "getFDVal");
    int fd = (Integer) fdVal.invoke(pipe.source());

    int epfd = (Integer) create.invoke(null);
    check(epfd >= 0, "epollCreate");
    check((Integer) ctl.invoke(null, epfd, 1 /* EPOLL_CTL_ADD */, fd, EPOLLIN) == 0, "epollCtl");
    long events = (Long) allocate.invoke(null, 4);

    pipe.sink().write(ByteBuffer.wrap(new byte[] {42}));
    check((Integer) wait.invoke(null, epfd, events, 4) == 1, "pipe readable after the write");
    check((Integer) getDescriptor.invoke(null, events) == fd, "the event names the pipe");
    check(((Integer) getEvents.invoke(null, events) & EPOLLIN) != 0, "EPOLLIN set");

    ByteBuffer in = ByteBuffer.allocate(1);
    check(pipe.source().read(in) == 1 && in.get(0) == 42, "the byte read back");
    System.out.println("EPollTest all passed");
  }

  private static Method method(Class<?> c, String name, Class<?>... params) throws Exception {
    Method m = c.getDeclaredMethod(name, params);
    m.setAccessible(true);
    return m;
  }

  private static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
  }
}
