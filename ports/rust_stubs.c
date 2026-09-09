#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "../include/syscall.h"

// Orthox-64 の実際のシステムコール関数の宣言
extern int _syscall_wrap(int num, uint64_t a1, uint64_t a2, uint64_t a3);

static int g_local_errno = 0;

// --- 根本的なシステムコール・ブリッジ ---
long syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret = (long)_syscall_wrap((int)num, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3);
    if (ret < 0) {
        g_local_errno = (int)-ret;
        return -1;
    }
    return ret;
}

// --- libc / std が期待する基本関数 ---
int *__errno_location(void) {
    return &g_local_errno;
}

// writev (ベクタ入出力を単純な write に変換)
struct iovec { void *base; size_t len; };
long writev(int fd, const struct iovec *iov, int iovcnt) {
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        long n = syscall(1 /* SYS_WRITE */, fd, (long)iov[i].base, (long)iov[i].len, 0, 0, 0);
        if (n < 0) return n;
        total += n;
    }
    return total;
}

char *realpath(const char *path, char *resolved_path) { return (char*)path; }
unsigned long getauxval(unsigned long type) { return 0; }
int mprotect(void *addr, size_t len, int prot) { return 0; }
int sigaltstack(const void *ss, void *oss) { return 0; }

// --- Unwind (例外処理) スタブ ---
void _Unwind_Resume(void *obj) { while(1); }
void _Unwind_DeleteException(void *obj) {}
int _Unwind_RaiseException(void *obj) { return 0; }
int _Unwind_Backtrace(void *callback, void *arg) { return 0; }
uintptr_t _Unwind_GetIP(void *obj) { return 0; }
uintptr_t _Unwind_GetIPInfo(void *obj, int *ip_before_insn) { return 0; }
uintptr_t _Unwind_GetRegionStart(void *obj) { return 0; }
uintptr_t _Unwind_GetDataRelBase(void *obj) { return 0; }
uintptr_t _Unwind_GetTextRelBase(void *obj) { return 0; }
void* _Unwind_GetLanguageSpecificData(void *obj) { return (void*)0; }
void _Unwind_SetGR(void *obj, int index, uintptr_t val) {}
void _Unwind_SetIP(void *obj, uintptr_t val) {}

// --- Pthread (スレッド) - 簡易 TSD 実装 ---
#define MAX_PTHREAD_KEYS 64
static void* g_tsd_storage[MAX_PTHREAD_KEYS];
static uint32_t g_next_key = 1; // 0 は無効なキー (KEY_SENTVAL) とされるため 1 から開始

int pthread_key_create(uint32_t *key, void (*destructor)(void*)) {
    if (g_next_key >= MAX_PTHREAD_KEYS) return EAGAIN;
    *key = g_next_key++;
    return 0;
}

int pthread_key_delete(uint32_t key) { return 0; }

void* pthread_getspecific(uint32_t key) {
    if (key == 0 || key >= g_next_key) return NULL;
    return g_tsd_storage[key];
}

int pthread_setspecific(uint32_t key, const void *value) {
    if (key == 0 || key >= g_next_key) return EINVAL;
    g_tsd_storage[key] = (void*)value;
    return 0;
}

unsigned long pthread_self(void) { return 1; }
int pthread_mutex_lock(void *m) { return 0; }
int pthread_mutex_unlock(void *m) { return 0; }
int pthread_mutex_init(void *m, void *a) { return 0; }
int pthread_mutex_destroy(void *m) { return 0; }
int pthread_cond_wait(void *c, void *m) { return 0; }
int pthread_cond_signal(void *c) { return 0; }
int pthread_cond_broadcast(void *c) { return 0; }
int pthread_cond_destroy(void *c) { return 0; }
int pthread_detach(unsigned long t) { return 0; }
int pthread_getattr_np(unsigned long t, void *a) { return -1; }
int pthread_attr_getguardsize(void *a, size_t *s) { return 0; }
int pthread_attr_getstack(void *a, void **addr, size_t *s) { return 0; }
int pthread_attr_destroy(void *a) { return 0; }
int pthread_setname_np(unsigned long t, const char *n) { return 0; }

// --- Socket/Networking stubs for libc/std ---
int socket(int domain, int type, int protocol) {
    return (int)syscall(SYS_SOCKET, (long)domain, (long)type, (long)protocol, 0, 0, 0);
}

int connect(int fd, const void *addr, uint32_t len) {
    return (int)syscall(42 /* SYS_CONNECT */, (long)fd, (long)addr, (long)len, 0, 0, 0);
}

int accept(int fd, void *addr, uint32_t *len) {
    return (int)syscall(SYS_ACCEPT, (long)fd, (long)addr, (long)len, 0, 0, 0);
}

int bind(int fd, const void *addr, uint32_t len) {
    return (int)syscall(SYS_BIND, (long)fd, (long)addr, (long)len, 0, 0, 0);
}

int listen(int fd, int backlog) {
    return (int)syscall(SYS_LISTEN, (long)fd, (long)backlog, 0, 0, 0, 0);
}

int getsockname(int fd, void *addr, uint32_t *len) {
    return (int)syscall(SYS_GETSOCKNAME, (long)fd, (long)addr, (long)len, 0, 0, 0);
}

int getpeername(int fd, void *addr, uint32_t *len) {
    return (int)syscall(SYS_GETPEERNAME, (long)fd, (long)addr, (long)len, 0, 0, 0);
}

int setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen) {
    return (int)syscall(SYS_SETSOCKOPT, (long)fd, (long)level, (long)optname, (long)optval, (long)optlen, 0);
}

int shutdown(int fd, int how) {
    return (int)syscall(SYS_SHUTDOWN, (long)fd, (long)how, 0, 0, 0, 0);
}

long send(int fd, const void *buf, size_t len, int flags) {
    return syscall(SYS_SENDTO, (long)fd, (long)buf, (long)len, (long)flags, 0, 0);
}

long sendto(int fd, const void *buf, size_t len, int flags, const void *addr, uint32_t addrlen) {
    return syscall(SYS_SENDTO, (long)fd, (long)buf, (long)len, (long)flags, (long)addr, (long)addrlen);
}

long recv(int fd, void *buf, size_t len, int flags) {
    return syscall(SYS_RECVFROM, (long)fd, (long)buf, (long)len, (long)flags, 0, 0);
}

long recvfrom(int fd, void *buf, size_t len, int flags, void *addr, uint32_t *addrlen) {
    return syscall(SYS_RECVFROM, (long)fd, (long)buf, (long)len, (long)flags, (long)addr, (long)addrlen);
}

// --- DNS lookup stubs to satisfy linkers ---
int getaddrinfo(const char *n, const char *s, const void *h, void **r) { return -1; }
void freeaddrinfo(void *r) {}
const char *gai_strerror(int e) { return "unknown error"; }
int res_init(void) { return 0; }

// --- Orthox-64 specific syscall bridges ---
int dns_lookup_ipv4(const char* hostname, uint32_t* out_addr) {
    return (int)syscall(1021 /* ORTH_SYS_DNS_LOOKUP */, (long)hostname, (long)out_addr, 0, 0, 0, 0);
}

// dl_iterate_phdr
int dl_iterate_phdr(int (*callback)(void *, size_t, void *), void *data) __attribute__((weak));
int dl_iterate_phdr(int (*callback)(void *, size_t, void *), void *data) { return 0; }

// --- 時間・同期 ---
int clock_gettime(int clk_id, void *tp) { return 0; }
int nanosleep(const void *req, void *rem) { return 0; }
int pause(void) { return 0; }
int sched_yield(void) { return 0; }

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    extern void *malloc(size_t);
    *memptr = malloc(size);
    return (*memptr == NULL) ? 12 : 0;
}
