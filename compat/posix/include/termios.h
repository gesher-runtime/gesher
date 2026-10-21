// gesher-base: <termios.h>. A Windows console is the only terminal: ECHO,
// ICANON and ISIG map to its input mode (src/misc.cc); on other fds the
// calls fail with ENOTTY.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

#define NCCS 32
struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[NCCS];
  speed_t c_ispeed;
  speed_t c_ospeed;
};

#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

int tcgetattr(int fd, struct termios* t);
int tcsetattr(int fd, int action, const struct termios* t);
int tcdrain(int fd);
int tcflush(int fd, int queue);
int tcsendbreak(int fd, int duration);
int tcflow(int fd, int action);

#ifdef __cplusplus
}
#endif
