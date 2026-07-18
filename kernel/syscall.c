#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "sys_internal.h"
#include "task.h"
#include "task_internal.h"
#include "fs.h"
#include "spinlock.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

extern void syscall_entry(void);

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init_cpu(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);
    uint64_t star = (0x10ULL << 48) | (0x08ULL << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);
}

void syscall_init(void) {
    syscall_init_cpu();
}

void syscall_dispatch(arch_syscall_frame_t* frame) {
    uint64_t syscall_no = frame->rax;
    struct task* current = get_current_task();
    (void)current;
    kernel_lock_enter();
    syscall_trace_progress_bump(current, &current->trace_syscalls);
    syscall_trace_progress_bump_syscall(current, syscall_no, frame->rdx);
    switch (syscall_no) {
        case SYS_READ:
            frame->rax = (uint64_t)sys_read((int)frame->rdi, (void*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_WRITE:
            frame->rax = (uint64_t)sys_write((int)frame->rdi, (const void*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_PREAD64:
            frame->rax = (uint64_t)sys_pread64((int)frame->rdi, (void*)frame->rsi,
                                               (size_t)frame->rdx, (int64_t)frame->r10);
            break;
        case SYS_PWRITE64:
            frame->rax = (uint64_t)sys_pwrite64((int)frame->rdi, (const void*)frame->rsi,
                                                (size_t)frame->rdx, (int64_t)frame->r10);
            break;
        case SYS_OPEN:
            frame->rax = (uint64_t)sys_open((const char*)frame->rdi, (int)frame->rsi, (int)frame->rdx);
            break;
        case SYS_OPENAT:
            frame->rax = (uint64_t)sys_openat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx, (int)frame->r10);
            break;
        case SYS_CLOSE:
            frame->rax = (uint64_t)sys_close((int)frame->rdi);
            break;
        case SYS_STAT:
            frame->rax = (uint64_t)sys_stat((const char*)frame->rdi, (struct kstat*)frame->rsi);
            break;
        case SYS_FSTAT:
            frame->rax = (uint64_t)sys_fstat((int)frame->rdi, (struct kstat*)frame->rsi);
            break;
        case SYS_LSTAT:
            frame->rax = (uint64_t)sys_lstat((const char*)frame->rdi, (struct kstat*)frame->rsi);
            break;
        case SYS_ACCESS:
            frame->rax = (uint64_t)sys_access((const char*)frame->rdi, (int)frame->rsi);
            break;
        case SYS_FSTATAT:
            frame->rax = (uint64_t)sys_fstatat((int)frame->rdi, (const char*)frame->rsi, (struct kstat*)frame->rdx, (int)frame->r10);
            break;
        case SYS_READLINK:
            frame->rax = (uint64_t)sys_readlink((const char*)frame->rdi, (char*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_READLINKAT:
            frame->rax = (uint64_t)sys_readlinkat((int)frame->rdi, (const char*)frame->rsi, (char*)frame->rdx, (size_t)frame->r10);
            break;
        case SYS_FACCESSAT:
            frame->rax = (uint64_t)sys_faccessat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx, (int)frame->r10);
            break;
        case SYS_UTIMENSAT:
            frame->rax = (uint64_t)sys_utimensat((int)frame->rdi, (const char*)frame->rsi,
                                                 (const void*)frame->rdx, (int)frame->r10);
            break;
        case SYS_LSEEK:
            frame->rax = (uint64_t)sys_lseek((int)frame->rdi, (int64_t)frame->rsi, (int)frame->rdx);
            break;
        case SYS_FTRUNCATE:
            frame->rax = (uint64_t)sys_ftruncate((int)frame->rdi, frame->rsi);
            break;
        case SYS_TRUNCATE:
            frame->rax = (uint64_t)sys_truncate((const char*)frame->rdi, frame->rsi);
            break;
        case SYS_MMAP:
            syscall_trace_progress_bump(current, &current->trace_mmap_calls);
            frame->rax = (uint64_t)sys_mmap((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, (int64_t)frame->r9);
            syscall_memtrace_mmap("mmap", frame->rdi, frame->rsi, frame->rax,
                                  (int)frame->rdx, (int)frame->r10, (int)frame->r8);
            break;
        case SYS_MREMAP:
            syscall_trace_progress_bump(current, &current->trace_mremap_calls);
            frame->rax = (uint64_t)sys_mremap((void*)frame->rdi, (size_t)frame->rsi, (size_t)frame->rdx,
                                              (int)frame->r10, (void*)frame->r8);
            syscall_memtrace_mmap("mremap", frame->rdi, frame->rdx, frame->rax,
                                  0, (int)frame->r10, -1);
            break;
        case SYS_MADVISE:
            frame->rax = (uint64_t)sys_madvise((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx);
            break;
        case SYS_MPROTECT:
            frame->rax = (uint64_t)sys_mprotect((void*)frame->rdi, (size_t)frame->rsi, (int)frame->rdx);
            syscall_memtrace_mmap("mprotect", frame->rdi, frame->rsi, frame->rax,
                                  (int)frame->rdx, 0, -1);
            break;
        case SYS_MUNMAP:
            syscall_trace_progress_bump(current, &current->trace_munmap_calls);
            frame->rax = (uint64_t)sys_munmap((void*)frame->rdi, (size_t)frame->rsi);
            syscall_memtrace_mmap("munmap", frame->rdi, frame->rsi, frame->rax,
                                  0, 0, -1);
            break;
        case SYS_IOCTL:
            frame->rax = (uint64_t)sys_ioctl((int)frame->rdi, (unsigned long)frame->rsi, frame->rdx);
            break;
        case SYS_WRITEV:
            frame->rax = (uint64_t)sys_writev((int)frame->rdi, (const struct orth_iovec*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_READV:
            frame->rax = (uint64_t)sys_readv((int)frame->rdi, (const struct orth_iovec*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_PIPE:
            frame->rax = (uint64_t)sys_pipe_user((int*)frame->rdi);
            break;
        case SYS_PIPE2:
            frame->rax = (uint64_t)sys_pipe2_user((int*)frame->rdi, (int)frame->rsi);
            break;
        case SYS_DUP2:
            frame->rax = (uint64_t)sys_dup2((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_SCHED_YIELD:
            frame->rax = (uint64_t)sys_sched_yield();
            break;
        case SYS_UNLINK:
            frame->rax = (uint64_t)sys_unlink((const char*)frame->rdi);
            break;
        case SYS_UNLINKAT:
            frame->rax = (uint64_t)sys_unlinkat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_RENAME:
            frame->rax = (uint64_t)sys_rename((const char*)frame->rdi, (const char*)frame->rsi);
            break;
        case SYS_CHMOD:
            frame->rax = (uint64_t)sys_chmod((const char*)frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_EXIT:
            sys_exit((int)frame->rdi);
            break;
        case SYS_BRK:
            syscall_trace_progress_bump(current, &current->trace_brk_calls);
            frame->rax = sys_brk(frame->rdi);
            break;
        case SYS_RT_SIGACTION:
            frame->rax = (uint64_t)sys_rt_sigaction((int)frame->rdi,
                                                    (const struct linux_rt_sigaction_k*)frame->rsi,
                                                    (struct linux_rt_sigaction_k*)frame->rdx,
                                                    (size_t)frame->r10);
            break;
        case SYS_RT_SIGPROCMASK:
            frame->rax = (uint64_t)sys_rt_sigprocmask((int)frame->rdi,
                                                      (const uint64_t*)frame->rsi,
                                                      (uint64_t*)frame->rdx,
                                                      (size_t)frame->r10);
            break;
        case SYS_NANOSLEEP:
            frame->rax = (uint64_t)sys_nanosleep((const struct orth_timespec_k*)frame->rdi,
                                                 (struct orth_timespec_k*)frame->rsi);
            break;
        case SYS_GETPID:
            frame->rax = sys_getpid();
            break;
        case SYS_GETPPID:
            frame->rax = sys_getppid();
            break;
        case SYS_GETUID:
            frame->rax = sys_getuid();
            break;
        case SYS_GETGID:
            frame->rax = sys_getgid();
            break;
        case SYS_GETEUID:
            frame->rax = sys_geteuid();
            break;
        case SYS_GETEGID:
            frame->rax = sys_getegid();
            break;
        case SYS_SOCKET:
            frame->rax = (uint64_t)sys_socket((int)frame->rdi, (int)frame->rsi, (int)frame->rdx);
            break;
        case SYS_CONNECT:
            frame->rax = (uint64_t)sys_connect((int)frame->rdi, (const void*)frame->rsi, (uint32_t)frame->rdx);
            break;
        case SYS_ACCEPT:
            frame->rax = (uint64_t)sys_accept((int)frame->rdi, (void*)frame->rsi, (uint32_t*)frame->rdx);
            break;
        case SYS_SENDTO:
            frame->rax = (uint64_t)sys_sendto((int)frame->rdi, (const void*)frame->rsi, (size_t)frame->rdx,
                                              (int)frame->r10, (const void*)frame->r8, (uint32_t)frame->r9);
            break;
        case SYS_RECVFROM:
            frame->rax = (uint64_t)sys_recvfrom((int)frame->rdi, (void*)frame->rsi, (size_t)frame->rdx,
                                                (int)frame->r10, (void*)frame->r8, (uint32_t*)frame->r9);
            break;
        case SYS_SHUTDOWN:
            frame->rax = (uint64_t)sys_shutdown((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_BIND:
            frame->rax = (uint64_t)sys_bind((int)frame->rdi, (const void*)frame->rsi, (uint32_t)frame->rdx);
            break;
        case SYS_LISTEN:
            frame->rax = (uint64_t)sys_listen((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_GETSOCKNAME:
            frame->rax = (uint64_t)sys_getsockname((int)frame->rdi, (void*)frame->rsi, (uint32_t*)frame->rdx);
            break;
        case SYS_GETPEERNAME:
            frame->rax = (uint64_t)sys_getpeername((int)frame->rdi, (void*)frame->rsi, (uint32_t*)frame->rdx);
            break;
        case SYS_SETSOCKOPT:
            frame->rax = (uint64_t)sys_setsockopt((int)frame->rdi, (int)frame->rsi, (int)frame->rdx,
                                                  (const void*)frame->r10, (uint32_t)frame->r8);
            break;
        case SYS_FORK:
            frame->rax = (uint64_t)task_fork(frame);
            break;
        case SYS_EXECVE:
            frame->rax = (uint64_t)task_execve(frame, (const char*)frame->rdi, (char* const*)frame->rsi, (char* const*)frame->rdx);
            break;
        case SYS_WAIT4:
            frame->rax = (uint64_t)sys_wait4((int)frame->rdi, (int*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_SYSINFO:
            frame->rax = (uint64_t)sys_sysinfo((struct linux_sysinfo_k*)frame->rdi);
            break;
        case SYS_GETCWD:
            frame->rax = (uint64_t)sys_getcwd((char*)frame->rdi, (size_t)frame->rsi);
            break;
        case SYS_CHDIR:
            frame->rax = (uint64_t)sys_chdir((const char*)frame->rdi);
            break;
        case SYS_FCHDIR:
            frame->rax = (uint64_t)sys_fchdir((int)frame->rdi);
            break;
        case SYS_MKDIR:
            frame->rax = (uint64_t)sys_mkdir((const char*)frame->rdi, (int)frame->rsi);
            break;
        case SYS_MKNOD:
            frame->rax = (uint64_t)sys_mknod((const char*)frame->rdi, (uint32_t)frame->rsi, frame->rdx);
            break;
        case SYS_PSELECT6:
            frame->rax = (uint64_t)sys_pselect6((int)frame->rdi, (uint64_t*)frame->rsi,
                                                (uint64_t*)frame->rdx, (uint64_t*)frame->r10,
                                                (const struct orth_timespec_k*)frame->r8,
                                                (const void*)frame->r9);
            break;
        case SYS_MKDIRAT:
            frame->rax = (uint64_t)sys_mkdirat((int)frame->rdi, (const char*)frame->rsi, (int)frame->rdx);
            break;
        case SYS_RMDIR:
            frame->rax = (uint64_t)sys_rmdir((const char*)frame->rdi);
            break;
        case SYS_ARCH_PRCTL:
            frame->rax = (uint64_t)sys_arch_prctl((int)frame->rdi, frame->rsi);
            break;
        case SYS_SYNC:
            frame->rax = (uint64_t)sys_sync();
            break;
        case SYS_FUTEX:
            frame->rax = (uint64_t)sys_futex((volatile int*)frame->rdi, (int)frame->rsi, (int)frame->rdx);
            break;
        case SYS_KILL:
            frame->rax = (uint64_t)sys_kill((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_UNAME:
            frame->rax = (uint64_t)sys_uname((struct linux_utsname_k*)frame->rdi);
            break;
        case SYS_GETRLIMIT:
            frame->rax = (uint64_t)sys_getrlimit((unsigned)frame->rdi, (struct linux_rlimit_k*)frame->rsi);
            break;
        case SYS_GETTIMEOFDAY:
            frame->rax = (uint64_t)sys_gettimeofday((struct orth_timeval_k*)frame->rdi);
            break;
        case SYS_GETPGRP:
            frame->rax = (uint64_t)sys_getpgrp();
            break;
        case SYS_CLOCK_GETTIME:
            frame->rax = (uint64_t)sys_clock_gettime((int)frame->rdi, (struct orth_timespec_k*)frame->rsi);
            break;
        case SYS_SETPGID:
            frame->rax = (uint64_t)sys_setpgid((int)frame->rdi, (int)frame->rsi);
            break;
        case SYS_SETSID:
            frame->rax = (uint64_t)sys_setsid();
            break;
        case ORTH_SYS_TCGETPGRP:
            frame->rax = (uint64_t)sys_tcgetpgrp((int)frame->rdi);
            break;
        case ORTH_SYS_TCSETPGRP:
            frame->rax = (uint64_t)sys_tcsetpgrp((int)frame->rdi, (int)frame->rsi);
            break;
        case ORTH_SYS_TCGETATTR:
            frame->rax = (uint64_t)sys_tcgetattr((int)frame->rdi, (struct orth_termios*)frame->rsi);
            break;
        case ORTH_SYS_TCSETATTR:
            frame->rax = (uint64_t)sys_tcsetattr((int)frame->rdi, (int)frame->rsi, (const struct orth_termios*)frame->rdx);
            break;
        case ORTH_SYS_SIGPROCMASK:
            frame->rax = (uint64_t)sys_sigprocmask((int)frame->rdi, (const uint64_t*)frame->rsi, (uint64_t*)frame->rdx);
            break;
        case ORTH_SYS_SIGPENDING:
            frame->rax = (uint64_t)sys_sigpending((uint64_t*)frame->rdi);
            break;
        case ORTH_SYS_SIGACTION:
            frame->rax = (uint64_t)sys_sigaction((int)frame->rdi, (const struct orth_sigaction*)frame->rsi, (struct orth_sigaction*)frame->rdx);
            break;
        case SYS_FCNTL:
            frame->rax = (uint64_t)sys_fcntl((int)frame->rdi, (int)frame->rsi, frame->rdx);
            break;
        case ORTH_SYS_LS:
            sys_ls_private();
            break;
        case ORTH_SYS_GET_VIDEO_INFO:
            frame->rax = (uint64_t)sys_get_video_info((struct video_info*)frame->rdi);
            break;
        case ORTH_SYS_MAP_FRAMEBUFFER:
            frame->rax = sys_map_framebuffer();
            break;
        case ORTH_SYS_GET_TICKS_MS:
            frame->rax = sys_get_ticks_ms();
            break;
        case ORTH_SYS_SLEEP_MS:
            frame->rax = (uint64_t)sys_sleep_ms(frame->rdi);
            break;
        case ORTH_SYS_GET_KEY_EVENT:
            frame->rax = (uint64_t)sys_get_key_event((struct key_event*)frame->rdi);
            break;
        case ORTH_SYS_SOUND_ON:
            frame->rax = (uint64_t)sys_sound_on((uint32_t)frame->rdi);
            break;
        case ORTH_SYS_SOUND_OFF:
            frame->rax = (uint64_t)sys_sound_off();
            break;
        case ORTH_SYS_SOUND_PCM_U8:
            frame->rax = (uint64_t)sys_sound_pcm_u8((const uint8_t*)frame->rdi, (uint32_t)frame->rsi, (uint32_t)frame->rdx);
            break;
        case ORTH_SYS_USB_INFO:
            frame->rax = (uint64_t)sys_usb_info();
            break;
        case ORTH_SYS_USB_READ_BLOCK:
            frame->rax = (uint64_t)sys_usb_read_block((uint32_t)frame->rdi, (void*)frame->rsi, (uint32_t)frame->rdx);
            break;
        case ORTH_SYS_MOUNT_USB_ROOT:
            frame->rax = (uint64_t)sys_mount_usb_root();
            break;
        case ORTH_SYS_MOUNT_MODULE_ROOT:
            frame->rax = (uint64_t)sys_mount_module_root();
            break;
        case ORTH_SYS_GET_MOUNT_STATUS:
            frame->rax = (uint64_t)sys_get_mount_status((char*)frame->rdi, (size_t)frame->rsi);
            break;
        case ORTH_SYS_DNS_LOOKUP:
            frame->rax = (uint64_t)sys_dns_lookup((const char*)frame->rdi, (uint32_t*)frame->rsi);
            break;
        case ORTH_SYS_GET_CPU_ID:
            frame->rax = (uint64_t)sys_get_cpu_id();
            break;
        case ORTH_SYS_SET_FORK_SPREAD:
            frame->rax = (uint64_t)sys_set_fork_spread((int)frame->rdi);
            break;
        case ORTH_SYS_GET_FORK_SPREAD:
            frame->rax = (uint64_t)sys_get_fork_spread();
            break;
        case ORTH_SYS_GET_RUNQ_STATS:
            frame->rax = (uint64_t)sys_get_runq_stats((struct orth_runq_stat*)frame->rdi, (uint32_t)frame->rsi);
            break;
        case SYS_GETDENTS:
            frame->rax = (uint64_t)sys_getdents((int)frame->rdi, (struct orth_dirent*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_GETDENTS64:
            frame->rax = (uint64_t)sys_getdents64((int)frame->rdi, (void*)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_SET_ROBUST_LIST:
            frame->rax = (uint64_t)sys_set_robust_list((const void*)frame->rdi, (size_t)frame->rsi);
            break;
        case SYS_SET_TID_ADDRESS:
            frame->rax = (uint64_t)sys_set_tid_address((int*)frame->rdi);
            break;
        case SYS_PRLIMIT64:
            frame->rax = (uint64_t)sys_prlimit64((int)frame->rdi, (unsigned)frame->rsi,
                                                 (const struct linux_rlimit_k*)frame->rdx,
                                                 (struct linux_rlimit_k*)frame->r10);
            break;
        case SYS_GETRANDOM:
            frame->rax = (uint64_t)sys_getrandom((void*)frame->rdi, (size_t)frame->rsi, (unsigned)frame->rdx);
            break;
        case SYS_SIGALTSTACK:
            frame->rax = (uint64_t)sys_sigaltstack((const struct linux_stack_t_k*)frame->rdi,
                                                   (struct linux_stack_t_k*)frame->rsi);
            break;
        case SYS_EXIT_GROUP:
            sys_exit((int)frame->rdi);
            break;
        default:
            frame->rax = (uint64_t)-38;
            break;
    }
    // Timer IRQ sets resched pending; perform context switch at syscall boundary,
    // not in interrupt return path, to avoid iretq frame corruption.
    if (task_consume_resched()) {
        kernel_yield();
    }
    kernel_lock_exit();
}
