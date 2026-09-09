#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fail(const char *msg) {
    printf("vmerrno_test: FAIL: %s errno=%d\n", msg, errno);
    return 1;
}

int main(void) {
    char *p;
    void *q;
    long sc_ret;

    errno = 0;
    if (open("/definitely/missing.file", O_RDONLY) != -1 || errno != ENOENT) {
        return fail("open missing should set ENOENT");
    }

    errno = 0;
    if (mmap(0, 4096, PROT_READ, MAP_PRIVATE, 123, 0) != MAP_FAILED || errno != EBADF) {
        return fail("file-backed mmap with bad fd should set EBADF");
    }

    p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return fail("anonymous mmap failed");
    }
    strcpy(p, "hello-vm");

    errno = 0;
    if (mprotect(p, 4096, PROT_READ) != 0) {
        return fail("mprotect read-only failed");
    }
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) {
        return fail("mprotect read-write failed");
    }
    strcat(p, "-ok");

    q = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (q == MAP_FAILED) {
        return fail("scratch mmap failed");
    }
    if (munmap(q, 4096) != 0) {
        return fail("scratch munmap failed");
    }

    errno = 0;
    if (mprotect(q, 4096, PROT_READ) != -1 || errno != ENOMEM) {
        return fail("mprotect unmapped should set ENOMEM");
    }

    q = mremap(p, 4096, 8192, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) {
        return fail("mremap grow failed");
    }
    p = q;
    if (strcmp(p, "hello-vm-ok") != 0) {
        errno = 0;
        return fail("mremap grow lost contents");
    }
    strcpy(p + 4096, "tail-page");

    errno = 0;
    if (mremap(p, 8192, 12288, MREMAP_FIXED, p) != MAP_FAILED || errno != EINVAL) {
        return fail("mremap fixed without maymove should set EINVAL");
    }

    q = mremap(p, 8192, 4096, 0);
    if (q == MAP_FAILED) {
        return fail("mremap shrink failed");
    }
    p = q;
    if (strcmp(p, "hello-vm-ok") != 0) {
        errno = 0;
        return fail("mremap shrink lost contents");
    }

    errno = 0;
    sc_ret = syscall(9999);
    if (sc_ret != -1 || errno != ENOSYS) {
        return fail("unsupported syscall should set ENOSYS");
    }

    if (munmap(p, 4096) != 0) {
        return fail("munmap failed");
    }

    printf("vmerrno_test: PASS\n");
    return 0;
}
