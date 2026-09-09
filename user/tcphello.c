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
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        say("tcphello: socket failed\n");
        return 1;
    }

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0x901F; /* htons(8080) */
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        say("tcphello: bind failed\n");
        close(fd);
        return 1;
    }
    if (listen(fd, 4) < 0) {
        say("tcphello: listen failed\n");
        close(fd);
        return 1;
    }

    say("tcphello: listening 8080\n");

    int client = accept(fd, 0, 0);
    if (client < 0) {
        say("tcphello: accept failed\n");
        close(fd);
        return 1;
    }

    char req[256];
    (void)read(client, req, sizeof(req));

    const char* resp =
        "HTTP/1.0 200 OK\r\n"
        "Content-Length: 12\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello World\n";
    if (write(client, resp, strlen(resp)) < 0) {
        say("tcphello: write failed\n");
        close(client);
        close(fd);
        return 1;
    }

    shutdown(client, SHUT_WR);
    close(client);
    close(fd);
    say("tcphello: served one client\n");
    return 0;
}
