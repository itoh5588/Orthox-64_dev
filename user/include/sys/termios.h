#ifndef _SYS_TERMIOS_H
#define _SYS_TERMIOS_H

#include <sys/types.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#ifndef NCCS
#define NCCS 20
#endif

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10

#define BRKINT   0x00000001u
#define ICRNL    0x00000002u
#define INLCR    0x00000004u
#define IUCLC    0x00000008u
#define IXON     0x00000010u
#define IXANY    0x00000020u
#define IXOFF    0x00000040u
#define IMAXBEL  0x00000080u

#define ONLCR    0x00000001u

#define ISIG     0x00000001u
#define ICANON   0x00000002u

#define B0       0
#define B50      50
#define B75      75
#define B110     110
#define B134     134
#define B150     150
#define B200     200
#define B300     300
#define B600     600
#define B1200    1200
#define B1800    1800
#define B2400    2400
#define B4800    4800
#define B9600    9600
#define B19200   19200
#define B38400   38400
#define B57600   57600
#define B115200  115200
#define B230400  230400

#define ECHO    0x00000008u
#define ECHOE   0x00000010u
#define ECHOK   0x00000020u
#define ECHONL  0x00000040u

int tcgetattr(int fd, struct termios* tio);
int tcsetattr(int fd, int optional_actions, const struct termios* tio);
int tcflush(int fd, int queue_selector);

#endif
