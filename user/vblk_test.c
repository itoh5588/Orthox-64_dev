#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

void sync(void);

int main(int argc, char* argv[]) {
    const char* filename = "/vblk_test.txt";
    char buf[256];
    int fd;

    if (argc > 1 && strcmp(argv[1], "read") == 0) {
        printf("Testing Read from VirtIO-Block...\n");
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            perror("open for read");
            return 1;
        }
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            perror("read");
            close(fd);
            return 1;
        }
        buf[n] = '\0';
        printf("Read content: [%s]\n", buf);
        close(fd);
        if (strcmp(buf, "Hello VirtIO-Block Persistence!") == 0) {
            printf("SUCCESS: Persistence verified!\n");
        } else {
            printf("FAILURE: Content mismatch.\n");
            return 1;
        }
    } else {
        printf("Testing Write to VirtIO-Block...\n");
        fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open for write");
            return 1;
        }
        const char* msg = "Hello VirtIO-Block Persistence!";
        if (write(fd, msg, strlen(msg)) < 0) {
            perror("write");
            close(fd);
            return 1;
        }
        close(fd);
        printf("Write successful. Syncing...\n");
        sync();
        printf("Done. Please reboot and run with 'read' argument.\n");
    }

    return 0;
}
