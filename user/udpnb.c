#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

static void say(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        say("udpnb: socket failed\n");
        return 1;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0x3C30; /* htons(12348) */
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        say("udpnb: bind failed\n");
        close(fd);
        return 1;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        say("udpnb: fcntl failed\n");
        close(fd);
        return 1;
    }

    char buf[16];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n >= 0) {
        say("udpnb: recv unexpectedly succeeded\n");
        close(fd);
        return 1;
    }
    if (errno != EAGAIN) {
        say("udpnb: expected EAGAIN\n");
        close(fd);
        return 1;
    }

    say("udpnb: PASS EAGAIN\n");
    close(fd);
    return 0;
}
