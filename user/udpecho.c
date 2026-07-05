#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>

static void say(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    write(1, s, n);
}

int main(void) {
    say("udpecho start\n");
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        say("socket failed\n");
        return 1;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0x3A30; /* htons(12346) */
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        say("bind failed\n");
        close(fd);
        return 1;
    }

    say("udpecho ready\n");

    char buf[256];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &peer_len);
    if (n < 0) {
        say("recvfrom failed\n");
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr*)&peer, peer_len) < 0) {
        say("connect failed\n");
        close(fd);
        return 1;
    }

    if (send(fd, buf, (size_t)n, 0) < 0) {
        say("send failed\n");
        close(fd);
        return 1;
    }

    write(1, buf, (size_t)n);
    write(1, "\n", 1);
    close(fd);
    return 0;
}
