#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static void fail(const char* msg) {
    printf("ttytest: FAIL: %s\n", msg);
}

int main(void) {
    struct termios orig;
    struct termios tmp;
    struct winsize ws;
    pid_t pgrp;
    pid_t fg;

    printf("--- TTY / Terminal Control Test ---\n");

    if (tcgetattr(0, &orig) != 0) {
        fail("tcgetattr failed");
        return 1;
    }
    printf("termios: iflag=%u oflag=%u lflag=%u ispeed=%u ospeed=%u\n",
           (unsigned)orig.c_iflag,
           (unsigned)orig.c_oflag,
           (unsigned)orig.c_lflag,
           (unsigned)orig.__c_ispeed,
           (unsigned)orig.__c_ospeed);

    tmp = orig;
    tmp.c_lflag ^= ECHO;
    if (tcsetattr(0, TCSANOW, &tmp) != 0) {
        fail("tcsetattr toggle failed");
        return 1;
    }
    memset(&tmp, 0, sizeof(tmp));
    if (tcgetattr(0, &tmp) != 0) {
        fail("tcgetattr roundtrip failed");
        return 1;
    }
    if (((orig.c_lflag ^ tmp.c_lflag) & ECHO) == 0) {
        fail("tcsetattr did not change ECHO bit");
        return 1;
    }
    if (tcsetattr(0, TCSANOW, &orig) != 0) {
        fail("tcsetattr restore failed");
        return 1;
    }

    if (ioctl(0, TIOCGWINSZ, &ws) != 0) {
        fail("TIOCGWINSZ failed");
        return 1;
    }
    printf("winsize: rows=%u cols=%u\n", (unsigned)ws.ws_row, (unsigned)ws.ws_col);
    if (ws.ws_row == 0 || ws.ws_col == 0) {
        fail("winsize returned zero");
        return 1;
    }

    pgrp = getpgrp();
    fg = tcgetpgrp(0);
    printf("pgrp=%d fg=%d\n", pgrp, fg);
    if (pgrp <= 0 || fg <= 0) {
        fail("invalid pgrp values");
        return 1;
    }
    if (tcsetpgrp(0, pgrp) != 0) {
        fail("tcsetpgrp failed");
        return 1;
    }
    fg = tcgetpgrp(0);
    if (fg != pgrp) {
        fail("foreground pgrp mismatch after tcsetpgrp");
        return 1;
    }

    printf("ttytest: PASS\n");
    return 0;
}
