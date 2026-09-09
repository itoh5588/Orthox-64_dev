#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

#define MAX_TOOL_ARGS 128
#define MAX_SRCS 16
#define MAX_OBJS 32
#define MAX_PATH 256

static int has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return 0;
    }
    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static void basename_no_ext(const char *path, char *out, size_t out_size) {
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *dot;
    size_t len;

    if (slash != NULL) {
        base = slash + 1;
    }

    dot = strrchr(base, '.');
    len = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
    if (len + 1 > out_size) {
        len = out_size - 1;
    }

    memcpy(out, base, len);
    out[len] = '\0';
}

static void make_absolute_path(const char *path, char *out, size_t out_size) {
    char input[MAX_PATH];
    char cwd[MAX_PATH];

    if (out_size == 0) {
        return;
    }
    strncpy(input, path, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    if (input[0] == '/') {
        strncpy(out, input, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL || cwd[0] == '\0') {
        strncpy(out, input, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    if (strcmp(cwd, "/") == 0) {
        snprintf(out, out_size, "/%s", input);
    } else {
        snprintf(out, out_size, "%s/%s", cwd, input);
    }
}

static int run_wait(const char *cmd, char *const argv[]) {
    int status;
    pid_t pid;

    printf("[gcc] Executing %s...\n", cmd);
    pid = fork();
    if (pid == 0) {
        execve(cmd, argv, environ);
        perror("execve");
        exit(127);
    }
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *srcs[MAX_SRCS];
    const char *objs[MAX_OBJS];
    char src_paths[MAX_SRCS][MAX_PATH];
    char obj_paths[MAX_OBJS][MAX_PATH];
    char asm_outs[MAX_SRCS][MAX_PATH];
    char obj_outs[MAX_SRCS][MAX_PATH];
    char exe_out_path[MAX_PATH];
    int src_count = 0;
    int obj_count = 0;
    const char *out = NULL;
    const char *exe_out = "a.out";
    int compile_only = 0;
    int asm_only = 0;
    char auto_base[MAX_PATH];
    char *cc1_args[MAX_TOOL_ARGS];
    char *ld_extra_args[MAX_TOOL_ARGS];
    int cc1_argc = 0;
    int ld_extra_argc = 0;
    int i;

    if (argc < 2) {
        printf("Usage: gcc [-c|-S] [-o output] [options] source.c [...]\n");
        return 1;
    }

    cc1_args[cc1_argc++] = "/bin/cc1";
    cc1_args[cc1_argc++] = "-quiet";
    cc1_args[cc1_argc++] = "-nostdinc";
    cc1_args[cc1_argc++] = "-I/usr/include";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            compile_only = 1;
            continue;
        }
        if (strcmp(argv[i], "-S") == 0) {
            asm_only = 1;
            continue;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "gcc: missing argument after -o\n");
                return 1;
            }
            out = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "-I", 2) == 0 || strncmp(argv[i], "-D", 2) == 0 ||
            strncmp(argv[i], "-U", 2) == 0 || strncmp(argv[i], "-O", 2) == 0 ||
            strncmp(argv[i], "-W", 2) == 0 || strncmp(argv[i], "-std=", 5) == 0 ||
            strcmp(argv[i], "-g") == 0) {
            if (cc1_argc + 1 >= MAX_TOOL_ARGS) {
                fprintf(stderr, "gcc: too many options\n");
                return 1;
            }
            cc1_args[cc1_argc++] = argv[i];
            continue;
        }
        if (strncmp(argv[i], "-L", 2) == 0 || strncmp(argv[i], "-l", 2) == 0) {
            if (ld_extra_argc + 1 >= MAX_TOOL_ARGS) {
                fprintf(stderr, "gcc: too many ld options\n");
                return 1;
            }
            ld_extra_args[ld_extra_argc++] = argv[i];
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "gcc: unsupported option: %s\n", argv[i]);
            return 1;
        }
        if (has_suffix(argv[i], ".c")) {
            if (src_count >= MAX_SRCS) {
                fprintf(stderr, "gcc: too many source files\n");
                return 1;
            }
            srcs[src_count++] = argv[i];
            continue;
        }
        if (has_suffix(argv[i], ".o")) {
            if (obj_count >= MAX_OBJS) {
                fprintf(stderr, "gcc: too many object files\n");
                return 1;
            }
            objs[obj_count++] = argv[i];
            continue;
        }
        fprintf(stderr, "gcc: unsupported input file: %s\n", argv[i]);
        return 1;
    }

    if (src_count == 0 && obj_count == 0) {
        fprintf(stderr, "gcc: no input files\n");
        return 1;
    }
    if ((compile_only || asm_only) && obj_count != 0) {
        fprintf(stderr, "gcc: object input is only supported when linking\n");
        return 1;
    }
    if (compile_only && src_count > 1 && out != NULL) {
        fprintf(stderr, "gcc: cannot specify -o with -c and multiple source files\n");
        return 1;
    }
    if (asm_only && src_count > 1 && out != NULL) {
        fprintf(stderr, "gcc: cannot specify -o with -S and multiple source files\n");
        return 1;
    }
    if (compile_only && asm_only) {
        fprintf(stderr, "gcc: -c and -S cannot be used together\n");
        return 1;
    }
    if (src_count == 0 && (compile_only || asm_only)) {
        fprintf(stderr, "gcc: no source files\n");
        return 1;
    }

    if (!compile_only && !asm_only && out != NULL) {
        exe_out = out;
    }

    printf("--- OrthOS GCC Pipeline Started ---\n");

    for (i = 0; i < obj_count; i++) {
        make_absolute_path(objs[i], obj_paths[i], sizeof(obj_paths[i]));
    }

    for (i = 0; i < src_count; i++) {
        make_absolute_path(srcs[i], src_paths[i], sizeof(src_paths[i]));
        basename_no_ext(srcs[i], auto_base, sizeof(auto_base));
        snprintf(asm_outs[i], sizeof(asm_outs[i]), "%s.s", auto_base);
        make_absolute_path(asm_outs[i], asm_outs[i], sizeof(asm_outs[i]));
        snprintf(obj_outs[i], sizeof(obj_outs[i]), "%s.o", auto_base);
        make_absolute_path(obj_outs[i], obj_outs[i], sizeof(obj_outs[i]));
    }

    if (asm_only) {
        if (src_count == 1 && out != NULL) {
            make_absolute_path(out, asm_outs[0], sizeof(asm_outs[0]));
        }
    } else if (compile_only) {
        if (src_count == 1 && out != NULL) {
            make_absolute_path(out, obj_outs[0], sizeof(obj_outs[0]));
        }
    }
    make_absolute_path(exe_out, exe_out_path, sizeof(exe_out_path));

    for (i = 0; i < src_count; i++) {
        if (cc1_argc + 4 >= MAX_TOOL_ARGS) {
            fprintf(stderr, "gcc: too many arguments for cc1\n");
            return 1;
        }
        char *cc1_run[MAX_TOOL_ARGS + 4];
        memcpy(cc1_run, cc1_args, sizeof(char *) * cc1_argc);
        cc1_run[cc1_argc] = src_paths[i];
        cc1_run[cc1_argc + 1] = "-o";
        cc1_run[cc1_argc + 2] = asm_outs[i];
        cc1_run[cc1_argc + 3] = NULL;
        if (run_wait("/bin/cc1", cc1_run) != 0) {
            fprintf(stderr, "gcc: cc1 failed for %s\n", srcs[i]);
            return 1;
        }
        if (asm_only) {
            printf("--- OrthOS GCC Pipeline Finished! Output: %s ---\n", asm_outs[i]);
            continue;
        }
        char *as_args[] = { "/bin/as", asm_outs[i], "-o", obj_outs[i], NULL };
        if (run_wait("/bin/as", as_args) != 0) {
            fprintf(stderr, "gcc: as failed for %s\n", srcs[i]);
            return 1;
        }
        if (compile_only) {
            printf("--- OrthOS GCC Pipeline Finished! Output: %s ---\n", obj_outs[i]);
            continue;
        }
    }

    if (asm_only || compile_only) {
        return 0;
    }

    char *ld_args[MAX_TOOL_ARGS + 32];
    int ld_argc = 0;
    ld_args[ld_argc++] = "/bin/ld";
    ld_args[ld_argc++] = "/crt0.o";
    for (i = 0; i < src_count; i++) {
        ld_args[ld_argc++] = obj_outs[i];
    }
    for (i = 0; i < obj_count; i++) {
        ld_args[ld_argc++] = obj_paths[i];
    }
    ld_args[ld_argc++] = "/syscalls.o";
    for (i = 0; i < ld_extra_argc; i++) {
        ld_args[ld_argc++] = ld_extra_args[i];
    }
    ld_args[ld_argc++] = "/usr/lib/libc.a";
    ld_args[ld_argc++] = "-o";
    ld_args[ld_argc++] = exe_out_path;
    ld_args[ld_argc] = NULL;

    if (run_wait("/bin/ld", ld_args) != 0) {
        fprintf(stderr, "gcc: ld failed\n");
        return 1;
    }

    for (i = 0; i < src_count; i++) {
        unlink(asm_outs[i]);
        unlink(obj_outs[i]);
    }

    printf("--- OrthOS GCC Pipeline Finished! Output: %s ---\n", exe_out);
    return 0;
}
