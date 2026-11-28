// The receive queue must hold the pipe writer until the last receiver alias closes.
#include "rights_probe.h"

static void test(int type) {
  int pair[2];
  int pipefd[2];
  if (socketpair(AF_UNIX, type, 0, pair) != 0) {
    check("create the Unix socket pair", 0);
    return;
  }
  if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0) {
    check("create the nonblocking pipe", 0);
    close_pair(pair);
    return;
  }

  int alias = dup(pair[1]);
  int queued = check("duplicate the receiving descriptor", alias >= 0) &&
      check("queue the sole pipe writer", send_fd(pair[0], 'q', pipefd[1]));
  close(pipefd[1]);
  close(pair[1]);
  if (queued) check_pipe(pipefd[0], "a receiver alias keeps the queued writer alive", 0);
  if (alias >= 0) close(alias);
  if (queued) check_pipe(pipefd[0], "last receiver close releases the writer", 1);
  close(pair[0]);
  close(pipefd[0]);
}

int main(void) { return run_probes(test); }
