#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <glob.h>
#ifdef sa_handler
#undef sa_handler
#endif
#include "syscall.h"

#ifndef ORTHOS_SH_ENABLE_USB
#define ORTHOS_SH_ENABLE_USB 1
#endif

extern int scandir(const char*, struct dirent***,
                   int (*)(const struct dirent*),
                   int (*)(const struct dirent**, const struct dirent**));

#define MAX_LINE 256
#define MAX_ARGS 16
#define HISTORY_SIZE 32
#define COMPLETION_MAX 64
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef O_CREAT
#define O_CREAT 0x0200
#endif
#ifndef O_TRUNC
#define O_TRUNC 0x0400
#endif

extern char **environ;
#if ORTHOS_SH_ENABLE_USB
static uint8_t g_usb_read_buf[4096];
static uint8_t g_usb_dir_bufs[8][4096];
static int g_usb_dir_buf_depth = 0;
#endif
static struct orth_dirent g_dirents_buf[16];
static char g_cwd[256] = "/";
static char g_env_path[] = "PATH=/usr/bin:/bin:/:/boot";
static char g_env_pwd[320] = "PWD=/";
static char g_env_home[] = "HOME=/";
static char* g_initial_envp[] = { g_env_path, g_env_pwd, g_env_home, NULL };
static pid_t g_shell_pgrp = 0;
static char g_history[HISTORY_SIZE][MAX_LINE];
static int g_history_count = 0;
static int g_history_next = 0;

#define USB_READ_RETRIES 3

static const char* g_builtin_names[] = {
    "pwd", "cd", "ls", "cat", "stat", "sync", "touch", "history",
    "mount", "muslcheck", "writecheck", "dircheck", "scandircheck",
    "globcheck",
#if ORTHOS_SH_ENABLE_USB
    "usb",
#endif
    "exit", NULL
};

static void shell_restore_foreground(void);
static void shell_init_job_control(void);
static int run_pipeline_or_redirect(int argc, char** args);
static int run_shell_command_once(char* line);
static int run_extended_builtin(int argc, char** args);
static void run_write_check(const char* path);
static void run_dir_check(const char* path);
static void run_scandir_check(const char* path);
static void run_glob_check(const char* pattern);
static void run_musl_check(void);
static void resolve_shell_path(const char* path, char* out, size_t out_size);
static int shell_exec_external(char* const argv[]);
static void shell_debug_exec_args(int argc, char** argv);

#if ORTHOS_SH_ENABLE_USB
static int usb_read_blocks_safe(uint32_t lba, void* buf, uint32_t count) {
    uint8_t* dst = (uint8_t*)buf;
    for (uint32_t done = 0; done < count; done++) {
        for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
            if (usb_read_block_sys(lba + done, dst + done * 512U, 1) == 0) break;
            if (attempt == USB_READ_RETRIES - 1) return -1;
        }
    }
    return 0;
}

struct fat_boot_info {
    uint32_t part_start;
    uint32_t part_sectors;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fats;
    uint32_t fat_size;
    uint32_t root_cluster;
    uint32_t data_start_lba;
};

struct fat_dir_entry_info {
    char name[256];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
};

struct fat_dir_iter_cb_ctx {
    const char* target;
    struct fat_dir_entry_info* out;
    int found;
};

struct fat_tree_ctx {
    const struct fat_boot_info* info;
    int depth;
};

struct fat_lfn_state {
    char name[256];
    int valid;
};
#endif

static void shell_write_str(const char* s) {
    if (!s) return;
    write(1, s, strlen(s));
}

static void render_line(const char* prompt, const char* buf, int len, int cursor, int* rendered_len) {
    char seq[32];
    int visible_len = len;
    if (!prompt) prompt = "";
    write(1, "\r", 1);
    shell_write_str(prompt);
    if (len > 0) write(1, buf, len);
    if (rendered_len && *rendered_len > visible_len) {
        int extra = *rendered_len - visible_len;
        for (int i = 0; i < extra; i++) write(1, " ", 1);
        visible_len += extra;
    }
    shell_write_str("\x1b[K");
    if (visible_len > cursor) {
        snprintf(seq, sizeof(seq), "\x1b[%dD", visible_len - cursor);
        shell_write_str(seq);
    }
    if (rendered_len) *rendered_len = len;
}

static void history_store(const char* line) {
    if (!line || !line[0]) return;
    if (g_history_count > 0) {
        int last = (g_history_next + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (strcmp(g_history[last], line) == 0) return;
    }
    strncpy(g_history[g_history_next], line, MAX_LINE - 1);
    g_history[g_history_next][MAX_LINE - 1] = '\0';
    g_history_next = (g_history_next + 1) % HISTORY_SIZE;
    if (g_history_count < HISTORY_SIZE) g_history_count++;
}

static int history_to_absolute_index(int rel_index) {
    if (rel_index < 0 || rel_index >= g_history_count) return -1;
    return (g_history_next - g_history_count + rel_index + HISTORY_SIZE) % HISTORY_SIZE;
}

static void history_print(void) {
    for (int i = 0; i < g_history_count; i++) {
        int idx = history_to_absolute_index(i);
        if (idx < 0) continue;
        printf("%2d  %s\n", i + 1, g_history[idx]);
    }
}

static const char* history_get_entry_1based(int n) {
    int idx;
    if (n <= 0 || n > g_history_count) return 0;
    idx = history_to_absolute_index(n - 1);
    if (idx < 0) return 0;
    return g_history[idx];
}

static const char* history_get_last(void) {
    if (g_history_count <= 0) return 0;
    return history_get_entry_1based(g_history_count);
}

static int history_expand(const char* line, char* out, size_t out_size) {
    const char* src = line;
    const char* replacement = 0;
    char* endptr;
    long n;

    if (!line || !out || out_size == 0) return -1;
    if (line[0] != '!') {
        strncpy(out, line, out_size - 1);
        out[out_size - 1] = '\0';
        return 0;
    }

    if (strcmp(line, "!!") == 0) {
        replacement = history_get_last();
    } else {
        n = strtol(line + 1, &endptr, 10);
        if (endptr && *endptr == '\0' && n > 0) {
            replacement = history_get_entry_1based((int)n);
        }
    }

    if (!replacement) return -1;
    strncpy(out, replacement, out_size - 1);
    out[out_size - 1] = '\0';
    printf("%s\n", out);
    return 0;
}

static int completion_append(char matches[][MAX_LINE], int count, const char* candidate) {
    if (!candidate || !candidate[0] || count >= COMPLETION_MAX) return count;
    for (int i = 0; i < count; i++) {
        if (strcmp(matches[i], candidate) == 0) return count;
    }
    strncpy(matches[count], candidate, MAX_LINE - 1);
    matches[count][MAX_LINE - 1] = '\0';
    return count + 1;
}

static void completion_sort(char matches[][MAX_LINE], int count) {
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(matches[i], matches[j]) > 0) {
                char tmp[MAX_LINE];
                memcpy(tmp, matches[i], sizeof(tmp));
                memcpy(matches[i], matches[j], MAX_LINE);
                memcpy(matches[j], tmp, MAX_LINE);
            }
        }
    }
}

static int path_join_for_completion(const char* dir, const char* name, char* out, size_t out_size) {
    if (!dir || !name || !out || out_size == 0) return -1;
    if (strcmp(dir, "/") == 0) snprintf(out, out_size, "/%s", name);
    else if (strcmp(dir, ".") == 0) snprintf(out, out_size, "%s", name);
    else snprintf(out, out_size, "%s/%s", dir, name);
    return 0;
}

static int completion_collect_commands(const char* prefix, char matches[][MAX_LINE]) {
    int count = 0;
    DIR* dir;
    struct dirent* ent;
    size_t prefix_len = prefix ? strlen(prefix) : 0;

    for (int i = 0; g_builtin_names[i]; i++) {
        if (prefix_len == 0 || strncmp(g_builtin_names[i], prefix, prefix_len) == 0) {
            count = completion_append(matches, count, g_builtin_names[i]);
        }
    }

    dir = opendir("/bin");
    if (!dir) return count;
    while ((ent = readdir(dir)) != NULL) {
        if (!ent->d_name[0] || strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (prefix_len == 0 || strncmp(ent->d_name, prefix, prefix_len) == 0) {
            count = completion_append(matches, count, ent->d_name);
        }
    }
    closedir(dir);
    return count;
}

static int completion_collect_paths(const char* prefix, char matches[][MAX_LINE]) {
    char resolved_dir[256];
    char dir_part[256];
    char name_part[256];
    char display_prefix[256];
    const char* slash;
    DIR* dir;
    struct dirent* ent;
    int count = 0;
    size_t name_len;

    if (!prefix) prefix = "";
    slash = strrchr(prefix, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - prefix);
        if (dir_len == 0) {
            strcpy(dir_part, "/");
            strcpy(display_prefix, "/");
        } else {
            if (dir_len >= sizeof(dir_part)) dir_len = sizeof(dir_part) - 1;
            memcpy(dir_part, prefix, dir_len);
            dir_part[dir_len] = '\0';
            strncpy(display_prefix, dir_part, sizeof(display_prefix) - 1);
            display_prefix[sizeof(display_prefix) - 1] = '\0';
        }
        strncpy(name_part, slash + 1, sizeof(name_part) - 1);
        name_part[sizeof(name_part) - 1] = '\0';
    } else {
        strcpy(dir_part, ".");
        display_prefix[0] = '\0';
        strncpy(name_part, prefix, sizeof(name_part) - 1);
        name_part[sizeof(name_part) - 1] = '\0';
    }

    resolve_shell_path(dir_part, resolved_dir, sizeof(resolved_dir));
    dir = opendir(resolved_dir);
    if (!dir) return 0;
    name_len = strlen(name_part);
    while ((ent = readdir(dir)) != NULL) {
        char candidate[MAX_LINE];
        char stat_path[256];
        struct stat st;
        if (!ent->d_name[0] || strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (name_len && strncmp(ent->d_name, name_part, name_len) != 0) continue;
        if (display_prefix[0]) {
            if (strcmp(display_prefix, "/") == 0) snprintf(candidate, sizeof(candidate), "/%s", ent->d_name);
            else snprintf(candidate, sizeof(candidate), "%s/%s", display_prefix, ent->d_name);
        } else {
            snprintf(candidate, sizeof(candidate), "%s", ent->d_name);
        }
        path_join_for_completion(resolved_dir, ent->d_name, stat_path, sizeof(stat_path));
        if (stat(stat_path, &st) == 0 && (st.st_mode & 0170000) == 0040000) {
            size_t len = strlen(candidate);
            if (len + 1 < sizeof(candidate)) {
                candidate[len] = '/';
                candidate[len + 1] = '\0';
            }
        }
        count = completion_append(matches, count, candidate);
    }
    closedir(dir);
    return count;
}

static int completion_longest_prefix(char matches[][MAX_LINE], int count) {
    int prefix_len;
    if (count <= 0) return 0;
    prefix_len = (int)strlen(matches[0]);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < prefix_len && matches[0][j] && matches[i][j] && matches[0][j] == matches[i][j]) j++;
        prefix_len = j;
    }
    return prefix_len;
}

static void apply_completion(const char* prompt, char* buf, int size, int* len, int* cursor, int* rendered_len) {
    int token_start = *cursor;
    int token_end = *cursor;
    char token[MAX_LINE];
    char matches[COMPLETION_MAX][MAX_LINE];
    int match_count;
    int completion_len;
    int is_command = 0;

    while (token_start > 0 && buf[token_start - 1] != ' ') token_start--;
    while (buf[token_end] && buf[token_end] != ' ') token_end++;
    if (token_end - token_start >= (int)sizeof(token)) return;
    memcpy(token, &buf[token_start], (size_t)(token_end - token_start));
    token[token_end - token_start] = '\0';
    is_command = (token_start == 0);

    if (is_command && strchr(token, '/') == NULL) {
        match_count = completion_collect_commands(token, matches);
    } else {
        match_count = completion_collect_paths(token, matches);
    }
    if (match_count <= 0) return;
    completion_sort(matches, match_count);

    if (match_count == 1) {
        completion_len = (int)strlen(matches[0]);
        if (token_start + completion_len + (token_end - *cursor) + 2 >= size) return;
        memmove(&buf[token_start + completion_len], &buf[token_end], (size_t)(*len - token_end + 1));
        memcpy(&buf[token_start], matches[0], (size_t)completion_len);
        *len = *len - (token_end - token_start) + completion_len;
        *cursor = token_start + completion_len;
        if ((*cursor == *len || buf[*cursor] == '\0') && matches[0][completion_len - 1] != '/' && *len + 1 < size) {
            memmove(&buf[*cursor + 1], &buf[*cursor], (size_t)(*len - *cursor + 1));
            buf[*cursor] = ' ';
            (*len)++;
            (*cursor)++;
        }
        render_line(prompt, buf, *len, *cursor, rendered_len);
        return;
    }

    completion_len = completion_longest_prefix(matches, match_count);
    if (completion_len > token_end - token_start) {
        int old_len = token_end - token_start;
        if (token_start + completion_len + (*len - token_end) + 1 < size) {
            memmove(&buf[token_start + completion_len], &buf[token_end], (size_t)(*len - token_end + 1));
            memcpy(&buf[token_start], matches[0], (size_t)completion_len);
            *len = *len - old_len + completion_len;
            *cursor = token_start + completion_len;
        }
        render_line(prompt, buf, *len, *cursor, rendered_len);
        return;
    }

    write(1, "\r\n", 2);
    for (int i = 0; i < match_count; i++) {
        char line[320];
        int width = 18;
        int len = snprintf(line, sizeof(line), "%-*s", width, matches[i]);
        write(1, line, len);
        if ((i % 4) == 3 || i == match_count - 1) {
            write(1, "\r\n", 2);
        }
    }
    render_line(prompt, buf, *len, *cursor, rendered_len);
}

static void get_line(const char* prompt, char* buf, int size) {
    int len = 0;
    int cursor = 0;
    int rendered_len = 0;
    int history_view = -1;
    char history_saved[MAX_LINE];

    buf[0] = '\0';
    history_saved[0] = '\0';
    while (len < size - 1) {
        char c;
        if (read(0, &c, 1) <= 0) break;

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            write(1, "\r\n", 2);
            break;
        }
        if (c == '\t') {
            apply_completion(prompt, buf, size, &len, &cursor, &rendered_len);
            continue;
        }
        if (c == 12) {
            shell_write_str("\x1b[2J\x1b[H");
            render_line(prompt, buf, len, cursor, &rendered_len);
            continue;
        }
        if (c == 1) {
            cursor = 0;
            render_line(prompt, buf, len, cursor, &rendered_len);
            continue;
        }
        if (c == 5) {
            cursor = len;
            render_line(prompt, buf, len, cursor, &rendered_len);
            continue;
        }
        if (c == '\b' || c == 0x7F) {
            if (cursor > 0) {
                memmove(&buf[cursor - 1], &buf[cursor], (size_t)(len - cursor + 1));
                cursor--;
                len--;
            }
            render_line(prompt, buf, len, cursor, &rendered_len);
            continue;
        }
        if ((unsigned char)c == 0x1b) {
            char seq[3];
            if (read(0, &seq[0], 1) <= 0) continue;
            if (read(0, &seq[1], 1) <= 0) continue;
            if (seq[0] != '[') continue;
            if (seq[1] == 'D') {
                if (cursor > 0) cursor--;
                render_line(prompt, buf, len, cursor, &rendered_len);
            } else if (seq[1] == 'C') {
                if (cursor < len) cursor++;
                render_line(prompt, buf, len, cursor, &rendered_len);
            } else if (seq[1] == 'H') {
                cursor = 0;
                render_line(prompt, buf, len, cursor, &rendered_len);
            } else if (seq[1] == 'F') {
                cursor = len;
                render_line(prompt, buf, len, cursor, &rendered_len);
            } else if (seq[1] == 'A') {
                if (g_history_count > 0) {
                    if (history_view < 0) {
                        strncpy(history_saved, buf, sizeof(history_saved) - 1);
                        history_saved[sizeof(history_saved) - 1] = '\0';
                        history_view = g_history_count - 1;
                    } else if (history_view > 0) {
                        history_view--;
                    }
                    strncpy(buf, g_history[history_to_absolute_index(history_view)], size - 1);
                    buf[size - 1] = '\0';
                    len = (int)strlen(buf);
                    cursor = len;
                    render_line(prompt, buf, len, cursor, &rendered_len);
                }
            } else if (seq[1] == 'B') {
                if (history_view >= 0) {
                    if (history_view < g_history_count - 1) {
                        history_view++;
                        strncpy(buf, g_history[history_to_absolute_index(history_view)], size - 1);
                    } else {
                        history_view = -1;
                        strncpy(buf, history_saved, size - 1);
                    }
                    buf[size - 1] = '\0';
                    len = (int)strlen(buf);
                    cursor = len;
                    render_line(prompt, buf, len, cursor, &rendered_len);
                }
            } else if (seq[1] == '3') {
                if (read(0, &seq[2], 1) <= 0) continue;
                if (seq[2] == '~' && cursor < len) {
                    memmove(&buf[cursor], &buf[cursor + 1], (size_t)(len - cursor));
                    len--;
                    render_line(prompt, buf, len, cursor, &rendered_len);
                }
            }
            continue;
        }
        if ((unsigned char)c < 0x20) continue;
        if (len >= size - 1) continue;
        memmove(&buf[cursor + 1], &buf[cursor], (size_t)(len - cursor + 1));
        buf[cursor] = c;
        cursor++;
        len++;
        render_line(prompt, buf, len, cursor, &rendered_len);
    }
    buf[len] = '\0';
    history_store(buf);
}

static void normalize_path_inplace(char* path) {
    char out[256];
    int oi = 0;
    int i = 0;
    if (!path || path[0] == '\0') return;
    out[oi++] = '/';
    out[oi] = '\0';
    while (path[i]) {
        char comp[64];
        int ci = 0;
        while (path[i] == '/') i++;
        while (path[i] && path[i] != '/' && ci < (int)sizeof(comp) - 1) {
            comp[ci++] = path[i++];
        }
        comp[ci] = '\0';
        if (ci == 0 || strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            if (oi > 1) {
                oi--;
                while (oi > 1 && out[oi - 1] != '/') oi--;
                out[oi] = '\0';
            }
            continue;
        }
        if (oi > 1 && out[oi - 1] != '/') out[oi++] = '/';
        for (int j = 0; comp[j] && oi < (int)sizeof(out) - 1; j++) out[oi++] = comp[j];
        out[oi] = '\0';
    }
    if (oi > 1 && out[oi - 1] == '/') out[oi - 1] = '\0';
    strncpy(path, out, 255);
    path[255] = '\0';
}

static void resolve_shell_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    if (path[0] == '/') {
        strncpy(out, path, out_size - 1);
        out[out_size - 1] = '\0';
    } else if (strcmp(g_cwd, "/") == 0) {
        snprintf(out, out_size, "/%s", path);
    } else {
        snprintf(out, out_size, "%s/%s", g_cwd, path);
    }
    normalize_path_inplace(out);
}

static void sync_shell_cwd(void) {
    if (!getcwd(g_cwd, sizeof(g_cwd))) {
        strncpy(g_cwd, "/", sizeof(g_cwd) - 1);
        g_cwd[sizeof(g_cwd) - 1] = '\0';
    }
}

static void sync_shell_env(void) {
    size_t i = 0;
    const char* cwd = g_cwd[0] ? g_cwd : "/";
    memcpy(g_env_pwd, "PWD=", 4);
    while (cwd[i] && i + 5 < sizeof(g_env_pwd)) {
        g_env_pwd[i + 4] = cwd[i];
        i++;
    }
    g_env_pwd[i + 4] = '\0';
}

#if ORTHOS_SH_ENABLE_USB
static int usb_get_first_partition(uint32_t* start, uint32_t* sectors) {
    uint8_t *e;
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_blocks_safe(0, g_usb_read_buf, 1) == 0) break;
        if (attempt == USB_READ_RETRIES - 1) return -1;
    }
    e = &g_usb_read_buf[446];
    *start = (uint32_t)e[8] |
             ((uint32_t)e[9] << 8) |
             ((uint32_t)e[10] << 16) |
             ((uint32_t)e[11] << 24);
    *sectors = (uint32_t)e[12] |
               ((uint32_t)e[13] << 8) |
               ((uint32_t)e[14] << 16) |
               ((uint32_t)e[15] << 24);
    return (*start != 0 && *sectors != 0) ? 0 : -1;
}

static int usb_load_fat_boot(struct fat_boot_info* info) {
    if (!info) return -1;
    if (usb_get_first_partition(&info->part_start, &info->part_sectors) < 0) return -1;
    for (int attempt = 0; attempt < USB_READ_RETRIES; attempt++) {
        if (usb_read_blocks_safe(info->part_start, g_usb_read_buf, 1) == 0) break;
        if (attempt == USB_READ_RETRIES - 1) return -1;
    }
    if (g_usb_read_buf[510] != 0x55 || g_usb_read_buf[511] != 0xAA) return -1;
    info->bytes_per_sector = (uint16_t)g_usb_read_buf[11] | ((uint16_t)g_usb_read_buf[12] << 8);
    info->sectors_per_cluster = g_usb_read_buf[13];
    info->reserved_sectors = (uint16_t)g_usb_read_buf[14] | ((uint16_t)g_usb_read_buf[15] << 8);
    info->fats = g_usb_read_buf[16];
    info->fat_size = (uint32_t)g_usb_read_buf[36] |
                     ((uint32_t)g_usb_read_buf[37] << 8) |
                     ((uint32_t)g_usb_read_buf[38] << 16) |
                     ((uint32_t)g_usb_read_buf[39] << 24);
    info->root_cluster = (uint32_t)g_usb_read_buf[44] |
                         ((uint32_t)g_usb_read_buf[45] << 8) |
                         ((uint32_t)g_usb_read_buf[46] << 16) |
                         ((uint32_t)g_usb_read_buf[47] << 24);
    if (info->bytes_per_sector != 512 || info->sectors_per_cluster == 0) return -1;
    info->data_start_lba = info->part_start + info->reserved_sectors + (uint32_t)info->fats * info->fat_size;
    return 0;
}

static uint32_t fat_cluster_to_lba(const struct fat_boot_info* info, uint32_t cluster) {
    return info->data_start_lba + (cluster - 2U) * (uint32_t)info->sectors_per_cluster;
}

static void fat_format_name(const uint8_t* e, char* name) {
    memcpy(name, e, 8);
    name[8] = '\0';
    while (name[0] && name[strlen(name) - 1] == ' ') name[strlen(name) - 1] = '\0';
    if (e[8] != ' ') {
        int n = strlen(name);
        name[n++] = '.';
        memcpy(&name[n], &e[8], 3);
        n += 3;
        name[n] = '\0';
        while (name[0] && name[strlen(name) - 1] == ' ') name[strlen(name) - 1] = '\0';
    }
}

static void fat_lfn_reset(struct fat_lfn_state* st) {
    if (!st) return;
    st->name[0] = '\0';
    st->valid = 0;
}

static void fat_lfn_append_chunk(struct fat_lfn_state* st, const uint8_t* e) {
    static const uint8_t offsets[] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    int order;
    int base;
    if (!st || !e) return;
    order = (e[0] & 0x1F);
    if (order <= 0) return;
    if (e[0] & 0x40) {
        fat_lfn_reset(st);
        st->valid = 1;
    }
    if (!st->valid) return;
    base = (order - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint16_t ch = (uint16_t)e[offsets[i]] | ((uint16_t)e[offsets[i] + 1] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) {
            st->name[base + i] = '\0';
            return;
        }
        st->name[base + i] = (ch < 0x80) ? (char)ch : '?';
    }
}

static uint32_t fat_read_next_cluster(const struct fat_boot_info* info, uint32_t cluster) {
    static uint32_t cached_lba = 0xFFFFFFFFU;
    static uint8_t cached_sector[512];
    static int cache_valid = 0;
    uint32_t fat_offset;
    uint32_t fat_lba;
    uint32_t ent_off;
    uint32_t next;
    if (!info || cluster < 2) return 0x0FFFFFFFU;
    fat_offset = cluster * 4U;
    fat_lba = info->part_start + info->reserved_sectors + (fat_offset / 512U);
    ent_off = fat_offset % 512U;
    if (!cache_valid || cached_lba != fat_lba) {
        if (usb_read_blocks_safe(fat_lba, cached_sector, 1) < 0) return 0x0FFFFFFFU;
        cached_lba = fat_lba;
        cache_valid = 1;
    }
    next = (uint32_t)cached_sector[ent_off] |
           ((uint32_t)cached_sector[ent_off + 1] << 8) |
           ((uint32_t)cached_sector[ent_off + 2] << 16) |
           ((uint32_t)cached_sector[ent_off + 3] << 24);
    return next & 0x0FFFFFFFU;
}

static void fat_decode_entry(const uint8_t* e, struct fat_dir_entry_info* ent) {
    ent->attr = e[11];
    fat_format_name(e, ent->name);
    ent->first_cluster = ((uint32_t)((uint16_t)e[20] | ((uint16_t)e[21] << 8)) << 16) |
                         (uint32_t)((uint16_t)e[26] | ((uint16_t)e[27] << 8));
    ent->size = (uint32_t)e[28] |
                ((uint32_t)e[29] << 8) |
                ((uint32_t)e[30] << 16) |
                ((uint32_t)e[31] << 24);
}

static void fat_decode_entry_name(const uint8_t* e, struct fat_lfn_state* lfn, struct fat_dir_entry_info* ent) {
    fat_decode_entry(e, ent);
    if (lfn && lfn->valid && lfn->name[0] != '\0') {
        strncpy(ent->name, lfn->name, sizeof(ent->name) - 1);
        ent->name[sizeof(ent->name) - 1] = '\0';
    }
    if (lfn) fat_lfn_reset(lfn);
}

static int fat_for_each_dir_entry(const struct fat_boot_info* info, uint32_t start_cluster,
                                  int (*cb)(const struct fat_dir_entry_info*, void*), void* cb_ctx) {
    uint32_t cluster;
    struct fat_lfn_state lfn;
    uint8_t* sector;
    if (!info || !cb || start_cluster < 2) return -1;
    if (g_usb_dir_buf_depth >= (int)(sizeof(g_usb_dir_bufs) / sizeof(g_usb_dir_bufs[0]))) return -1;
    sector = g_usb_dir_bufs[g_usb_dir_buf_depth++];
    fat_lfn_reset(&lfn);
    cluster = start_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8U) {
        uint32_t lba = fat_cluster_to_lba(info, cluster);
        uint32_t total = (uint32_t)info->bytes_per_sector * info->sectors_per_cluster;
        if (total > 4096U) {
            g_usb_dir_buf_depth--;
            return -1;
        }
        if (usb_read_blocks_safe(lba, sector, info->sectors_per_cluster) < 0) {
            g_usb_dir_buf_depth--;
            return -1;
        }
        for (uint32_t off = 0; off < (uint32_t)info->bytes_per_sector * info->sectors_per_cluster; off += 32) {
            uint8_t* e = &sector[off];
            struct fat_dir_entry_info ent;
            if (e[0] == 0x00) {
                g_usb_dir_buf_depth--;
                return 0;
            }
            if (e[0] == 0xE5) {
                fat_lfn_reset(&lfn);
                continue;
            }
            if (e[11] == 0x0F) {
                fat_lfn_append_chunk(&lfn, e);
                continue;
            }
            fat_decode_entry_name(e, &lfn, &ent);
            if (cb(&ent, cb_ctx) != 0) {
                g_usb_dir_buf_depth--;
                return 1;
            }
        }
        cluster = fat_read_next_cluster(info, cluster);
    }
    g_usb_dir_buf_depth--;
    return 0;
}

static int fat_find_entry_cb(const struct fat_dir_entry_info* ent, void* opaque) {
    struct fat_dir_iter_cb_ctx* ctx = (struct fat_dir_iter_cb_ctx*)opaque;
    if (strcmp(ent->name, ctx->target) != 0) return 0;
    *ctx->out = *ent;
    ctx->found = 1;
    return 1;
}

static int fat_read_dir_entry(const struct fat_boot_info* info, uint32_t dir_cluster,
                              const char* target, struct fat_dir_entry_info* out) {
    struct fat_dir_iter_cb_ctx ctx;
    if (!info || !target || !out) return -1;
    ctx.target = target;
    ctx.out = out;
    ctx.found = 0;
    if (fat_for_each_dir_entry(info, dir_cluster, fat_find_entry_cb, &ctx) < 0) return -1;
    return ctx.found ? 0 : -1;
}

static int fat_resolve_path(const struct fat_boot_info* info, const char* path, struct fat_dir_entry_info* out) {
    char path_buf[128];
    char* p;
    uint32_t dir_cluster;
    struct fat_dir_entry_info ent;
    if (!info || !path || !out || !path[0]) return -1;
    strncpy(path_buf, path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';
    dir_cluster = info->root_cluster;
    p = path_buf;
    while (*p == '/') p++;
    while (*p) {
        char* part = p;
        while (*p && *p != '/') p++;
        if (*p) {
            *p = '\0';
            p++;
            while (*p == '/') p++;
        }
        if (fat_read_dir_entry(info, dir_cluster, part, &ent) < 0) return -1;
        dir_cluster = ent.first_cluster;
        *out = ent;
    }
    return 0;
}

static int fat_print_entry_cb(const struct fat_dir_entry_info* ent, void* opaque) {
    (void)opaque;
    if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) return 0;
    printf("%-12s attr=%02x cluster=%u size=%u\n", ent->name, ent->attr, ent->first_cluster, ent->size);
    return 0;
}

static void fat_print_indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void sh_write_str(const char* s) {
    if (!s) return;
    write(1, s, strlen(s));
}

static void sh_write_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        write(1, "  ", 2);
    }
}

static int fat_tree_cb(const struct fat_dir_entry_info* ent, void* opaque) {
    struct fat_tree_ctx* ctx = (struct fat_tree_ctx*)opaque;
    if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) return 0;

    sh_write_indent(ctx->depth);
    sh_write_str(ent->name);
    if (ent->attr & 0x10U) write(1, "/", 1);
    write(1, "\n", 1);

    if ((ent->attr & 0x10U) == 0 || ctx->depth >= 4) return 0;
    ctx->depth++;
    (void)fat_for_each_dir_entry(ctx->info, ent->first_cluster, fat_tree_cb, ctx);
    ctx->depth--;
    return 0;
}

static const char* usb_strip_mount_prefix(const char* path) {
    if (!path) return 0;
    if (strcmp(path, "/usb") == 0 || strcmp(path, "/usb/") == 0) return "";
    if (strncmp(path, "/usb/", 5) == 0) return path + 5;
    if (strcmp(path, "usb") == 0 || strcmp(path, "usb/") == 0) return "";
    if (strncmp(path, "usb/", 4) == 0) return path + 4;
    return 0;
}
#endif

static int print_dir_listing(const char* path) {
    int fd;
    int n;
    char line[320];
    fd = open(path ? path : "/", O_DIRECTORY);
    if (fd < 0) return -1;
    while ((n = getdents_sys(fd, g_dirents_buf, sizeof(g_dirents_buf))) > 0) {
        int count = n / (int)sizeof(struct orth_dirent);
        for (int i = 0; i < count; i++) {
            int len = snprintf(line, sizeof(line), "%s%s\n",
                               g_dirents_buf[i].name,
                               (g_dirents_buf[i].mode == S_IFDIR) ? "/" : "");
            write(1, line, len);
        }
    }
    close(fd);
    return 0;
}

static int read_file_exact(const char* path, char* buf, size_t size) {
    int fd;
    int n;
    char resolved[256];
    if (!path || !buf || size == 0) return -1;
    resolve_shell_path(path, resolved, sizeof(resolved));
    fd = open(resolved, 0);
    if (fd < 0) return -1;
    n = read(fd, buf, (int)(size - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

static int shell_chdir(const char* path) {
    if (!path || !path[0]) return -1;
    if (chdir(path) < 0) return -1;
    sync_shell_cwd();
    sync_shell_env();
    return 0;
}

static int run_builtin(int argc, char** args) {
    char path_buf[256];
    if (argc <= 0 || !args[0]) return -1;
    if (strcmp(args[0], "pwd") == 0) {
        sync_shell_cwd();
        write(1, g_cwd, strlen(g_cwd));
        write(1, "\n", 1);
        return 0;
    }
    if (strcmp(args[0], "cd") == 0) {
        const char* target = (argc >= 2) ? args[1] : "/";
        if (shell_chdir(target) < 0) {
            printf("cd: %s: No such directory\n", target);
            return 1;
        }
        return 0;
    }
    if (strcmp(args[0], "ls") == 0) {
        const char* path = (argc >= 2) ? args[1] : g_cwd;
        resolve_shell_path(path, path_buf, sizeof(path_buf));
        if (print_dir_listing(path_buf) < 0) {
            if (strcmp(path_buf, "/") == 0) {
                __asm__ volatile ("mov %0, %%rax\nsyscall" : : "i"(ORTH_SYS_LS) : "rax", "rcx", "r11");
            } else {
                printf("ls: cannot access '%s'\n", path);
                return 1;
            }
        }
        return 0;
    }
    if (strcmp(args[0], "cat") == 0 && argc > 1) {
        int fd;
        char buf[256];
        int n;
        resolve_shell_path(args[1], path_buf, sizeof(path_buf));
        fd = open(path_buf, 0);
        if (fd < 0) {
            printf("cat: %s: No such file\n", args[1]);
            return 1;
        }
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            write(1, buf, n);
        }
        close(fd);
        write(1, "\n", 1);
        return 0;
    }
    if (strcmp(args[0], "cat") == 0) {
        char buf[256];
        int n;
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        return 0;
    }
    if (strcmp(args[0], "stat") == 0 && argc > 1) {
        struct stat st;
        const char* kind = "other";
        resolve_shell_path(args[1], path_buf, sizeof(path_buf));
        if (stat(path_buf, &st) != 0) {
            printf("stat: %s: No such file\n", args[1]);
            return 1;
        }
        if ((st.st_mode & 0170000) == 0040000) kind = "dir";
        else if ((st.st_mode & 0170000) == 0100000) kind = "file";
        else if ((st.st_mode & 0170000) == 0020000) kind = "chr";
        printf("path: %s\n", path_buf);
        printf("type: %s\n", kind);
        printf("mode: %o\n", st.st_mode);
        printf("size: %ld\n", (long)st.st_size);
        return 0;
    }
    if (strcmp(args[0], "sync") == 0) {
        sync();
        printf("sync: flushed filesystem state\n");
        return 0;
    }
    if (strcmp(args[0], "touch") == 0 && argc > 2) {
        int fd;
        resolve_shell_path(args[1], path_buf, sizeof(path_buf));
        fd = open(path_buf, 0x41);
        if (fd < 0) {
            printf("touch: failed to create %s\n", args[1]);
            return 1;
        }
        write(fd, args[2], strlen(args[2]));
        close(fd);
        printf("File '%s' updated.\n", path_buf);
        return 0;
    }
    if (strcmp(args[0], "history") == 0) {
        history_print();
        return 0;
    }
    return -1;
}

static int run_command_child(int argc, char** args) {
    int builtin = run_builtin(argc, args);
    if (builtin >= 0) {
        fflush(stdout);
        fflush(stderr);
        return builtin;
    }
    args[argc] = NULL;
    if (shell_exec_external(args) < 0) {
        perror(args[0]);
    }
    fflush(stdout);
    fflush(stderr);
    return 1;
}

static int shell_exec_external(char* const argv[]) {
    const char* path_env;
    const char* path_scan;
    int saved_errno = ENOENT;
    if (!argv || !argv[0] || !argv[0][0]) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(argv[0], '/')) {
        execve(argv[0], argv, environ);
        return -1;
    }

    path_env = getenv("PATH");
    if (!path_env || !path_env[0]) path_env = "/bin:/usr/bin:/";
    path_scan = path_env;

    while (1) {
        char candidate[256];
        const char* next = strchr(path_scan, ':');
        size_t entry_len = next ? (size_t)(next - path_scan) : strlen(path_scan);

        if (entry_len == 0) {
            snprintf(candidate, sizeof(candidate), "./%s", argv[0]);
        } else if (entry_len == 1 && path_scan[0] == '/') {
            snprintf(candidate, sizeof(candidate), "/%s", argv[0]);
        } else {
            int n = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)entry_len, path_scan, argv[0]);
            if (n < 0 || (size_t)n >= sizeof(candidate)) {
                errno = ENAMETOOLONG;
                return -1;
            }
        }

        execve(candidate, argv, environ);
        if (errno != ENOENT && errno != ENOTDIR) {
            saved_errno = errno;
        }

        if (!next) break;
        path_scan = next + 1;
    }

    errno = saved_errno;
    return -1;
}

static void shell_debug_exec_args(int argc, char** argv) {
    const char* debug = getenv("ORTHOS_SH_DEBUG");
    if (!debug || strcmp(debug, "0") == 0) return;
    printf("[sh.debug] argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("[sh.debug] argv[%d]='%s'\n", i, argv[i] ? argv[i] : "(null)");
    }
}

static int wait_status_to_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int run_shell_command_once(char* line) {
    char expanded[MAX_LINE];
    if (!line || strlen(line) == 0) return 0;
    if (strcmp(line, "exit") == 0) return 0;
    if (line[0] == '!') {
        if (history_expand(line, expanded, sizeof(expanded)) < 0) {
            printf("history: event not found: %s\n", line);
            return 1;
        }
        line = expanded;
    }

    char *args[MAX_ARGS];
    int argc = 0;
    char *token = strtok(line, " ");
    while (token && argc < MAX_ARGS - 1) {
        args[argc++] = token;
        token = strtok(NULL, " ");
    }
    args[argc] = NULL;

    if (argc == 0 || args[0] == NULL) {
        return 0;
    }

    {
        int special = run_pipeline_or_redirect(argc, args);
        if (special != -2) return special;
    }

    {
        int builtin = run_builtin(argc, args);
        if (builtin >= 0) return builtin;
    }

    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        exit(run_command_child(argc, args));
    } else if (pid > 0) {
        int status;
        (void)setpgid(pid, pid);
        (void)tcsetpgrp(0, pid);
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            shell_restore_foreground();
            return 1;
        }
        shell_restore_foreground();
        return wait_status_to_exit_code(status);
    }

    perror("fork");
    return 1;
}

static void shell_setup_environment(void) {
    setenv("PATH", "/bin:/:/usr/bin:/boot", 1);
    setenv("HOME", "/", 1);
    sync_shell_cwd();
    sync_shell_env();
    setenv("PWD", g_cwd, 1);
    shell_init_job_control();
}

static int run_extended_builtin(int argc, char** args) {
    if (strcmp(args[0], "mount") == 0) {
        char status[256];
        if (argc == 1) {
            if (get_mount_status(status, sizeof(status)) == 0) {
                printf("%s\n", status);
            } else {
                printf("mount: status unavailable\n");
            }
            return 0;
        }
        if (argc >= 2 && strcmp(args[1], "module") == 0) {
            if (mount_module_root() == 0) {
                sync_shell_cwd();
                printf("mount: root switched to boot module image\n");
            } else {
                printf("mount: failed to switch root to module\n");
            }
            return 0;
        }
        printf("mount: usage: mount [module]\n");
        return 0;
    }

    if (strcmp(args[0], "muslcheck") == 0) {
        run_musl_check();
        return 0;
    }
    if (strcmp(args[0], "writecheck") == 0) {
        run_write_check((argc >= 2) ? args[1] : "scratch.txt");
        return 0;
    }

    if (strcmp(args[0], "dircheck") == 0) {
        run_dir_check((argc >= 2) ? args[1] : "/");
        return 0;
    }

    if (strcmp(args[0], "scandircheck") == 0) {
        run_scandir_check((argc >= 2) ? args[1] : "/");
        return 0;
    }

    if (strcmp(args[0], "globcheck") == 0) {
        run_glob_check((argc >= 2) ? args[1] : "/usb/*");
        return 0;
    }

    return -1;
}

static void run_shell_line(char* line) {
    char expanded[MAX_LINE];
    if (!line || strlen(line) == 0) return;
    if (strcmp(line, "exit") == 0) return;
    if (line[0] == '!') {
        if (history_expand(line, expanded, sizeof(expanded)) < 0) {
            printf("history: event not found: %s\n", line);
            return;
        }
        line = expanded;
    }

    char *args[MAX_ARGS];
    int argc = 0;
    char *token = strtok(line, " ");
    while (token && argc < MAX_ARGS - 1) {
        args[argc++] = token;
        token = strtok(NULL, " ");
    }
    args[argc] = NULL;

    if (argc == 0 || args[0] == NULL) {
        return;
    }

    {
        int special = run_pipeline_or_redirect(argc, args);
        if (special != -2) return;
    }

    {
        int builtin = run_builtin(argc, args);
        if (builtin >= 0) return;
    }

    {
        int extended = run_extended_builtin(argc, args);
        if (extended >= 0) return;
    }

    if (strcmp(args[0], "usb") == 0) {
#if ORTHOS_SH_ENABLE_USB
        if (argc >= 2 && strcmp(args[1], "mbr") == 0) {
            if (usb_read_block_sys(0, g_usb_read_buf, 1) < 0) {
                printf("usb: failed to read MBR\n");
                return;
            }
            printf("mbr signature: %02x%02x\n", g_usb_read_buf[510], g_usb_read_buf[511]);
            for (int part = 0; part < 4; part++) {
                uint8_t *e = &g_usb_read_buf[446 + part * 16];
                uint32_t start = (uint32_t)e[8] |
                                 ((uint32_t)e[9] << 8) |
                                 ((uint32_t)e[10] << 16) |
                                 ((uint32_t)e[11] << 24);
                uint32_t sectors = (uint32_t)e[12] |
                                   ((uint32_t)e[13] << 8) |
                                   ((uint32_t)e[14] << 16) |
                                   ((uint32_t)e[15] << 24);
                if (e[4] == 0 || sectors == 0) continue;
                printf("part%d: boot=%02x type=%02x start=%u sectors=%u\n",
                       part + 1, e[0], e[4], start, sectors);
            }
            return;
        }
        if (argc >= 2 && strcmp(args[1], "fat") == 0) {
            struct fat_boot_info info;
            {
                char oem[9];
                char label[12];
                char fstype[9];
                if (usb_load_fat_boot(&info) < 0) {
                    printf("usb: failed to load FAT boot sector\n");
                    return;
                }
                memcpy(oem, &g_usb_read_buf[3], 8);
                oem[8] = '\0';
                memcpy(label, &g_usb_read_buf[71], 11);
                label[11] = '\0';
                memcpy(fstype, &g_usb_read_buf[82], 8);
                fstype[8] = '\0';
                printf("fat: start=%u sectors=%u oem='%s' label='%s' type='%s'\n",
                       info.part_start, info.part_sectors, oem, label, fstype);
                printf("fat: bps=%u spc=%u reserved=%u fats=%u fatsz=%u root_cluster=%u\n",
                       info.bytes_per_sector, info.sectors_per_cluster, info.reserved_sectors,
                       info.fats, info.fat_size, info.root_cluster);
            }
            return;
        }
        if (argc >= 2 && strcmp(args[1], "ls") == 0) {
            struct fat_boot_info info;
            uint32_t dir_cluster = 0;
            struct fat_dir_entry_info ent;
            if (usb_load_fat_boot(&info) < 0) {
                printf("usb: failed to load FAT boot sector\n");
                return;
            }
            if (argc >= 3) {
                if (fat_resolve_path(&info, args[2], &ent) < 0) {
                    printf("usb: path not found: %s\n", args[2]);
                    return;
                }
                if ((ent.attr & 0x10U) == 0) {
                    printf("usb: not a directory: %s\n", args[2]);
                    return;
                }
                dir_cluster = ent.first_cluster;
            } else {
                dir_cluster = info.root_cluster;
            }
            if (fat_for_each_dir_entry(&info, dir_cluster, fat_print_entry_cb, 0) < 0) {
                printf("usb: failed to read directory cluster=%u\n", dir_cluster);
            }
            return;
        }
        if (argc >= 2 && strcmp(args[1], "stat") == 0) {
            struct fat_boot_info info;
            struct fat_dir_entry_info ent;
            if (argc < 3) {
                printf("usb: usage: usb stat <path>\n");
                return;
            }
            if (usb_load_fat_boot(&info) < 0) {
                printf("usb: failed to load FAT boot sector\n");
                return;
            }
            if (fat_resolve_path(&info, args[2], &ent) < 0) {
                printf("usb: path not found: %s\n", args[2]);
                return;
            }
            printf("name: %s\n", ent.name);
            printf("type: %s\n", (ent.attr & 0x10U) ? "directory" : "file");
            printf("attr: %02x\n", ent.attr);
            printf("cluster: %u\n", ent.first_cluster);
            printf("size: %u\n", ent.size);
            return;
        }
        if (argc >= 2 && strcmp(args[1], "tree") == 0) {
            struct fat_boot_info info;
            struct fat_dir_entry_info ent;
            struct fat_tree_ctx tree;
            uint32_t dir_cluster;
            if (usb_load_fat_boot(&info) < 0) {
                printf("usb: failed to load FAT boot sector\n");
                return;
            }
            tree.info = &info;
            tree.depth = 0;
            if (argc >= 3) {
                if (fat_resolve_path(&info, args[2], &ent) < 0) {
                    printf("usb: path not found: %s\n", args[2]);
                    return;
                }
                if ((ent.attr & 0x10U) == 0) {
                    printf("usb: not a directory: %s\n", args[2]);
                    return;
                }
                printf("%s/\n", args[2]);
                dir_cluster = ent.first_cluster;
                tree.depth = 1;
            } else {
                printf("/\n");
                dir_cluster = info.root_cluster;
            }
            if (fat_for_each_dir_entry(&info, dir_cluster, fat_tree_cb, &tree) < 0) {
                printf("usb: failed to read tree at cluster=%u\n", dir_cluster);
            }
            return;
        }
        if (argc >= 3 && strcmp(args[1], "cat") == 0) {
            struct fat_boot_info info;
            struct fat_dir_entry_info ent;
            uint32_t cluster;
            uint32_t remaining;
            uint8_t last_byte = 0;
            if (usb_load_fat_boot(&info) < 0) {
                printf("usb: failed to load FAT boot sector\n");
                return;
            }
            if (fat_resolve_path(&info, args[2], &ent) < 0) {
                printf("usb: file not found: %s\n", args[2]);
                return;
            }
            if (ent.attr & 0x10U) {
                printf("usb: is a directory: %s\n", args[2]);
                return;
            }
            cluster = ent.first_cluster;
            remaining = ent.size;
            while (remaining > 0 && cluster >= 2 && cluster < 0x0FFFFFF8U) {
                uint32_t lba = fat_cluster_to_lba(&info, cluster);
                uint32_t chunk = info.bytes_per_sector * (uint32_t)info.sectors_per_cluster;
                if (chunk > remaining) chunk = remaining;
                if (usb_read_blocks_safe(lba, g_usb_read_buf, info.sectors_per_cluster) < 0) {
                    printf("usb: failed to read file cluster at lba=%u\n", lba);
                    break;
                }
                fwrite(g_usb_read_buf, 1, chunk, stdout);
                if (chunk > 0) last_byte = g_usb_read_buf[chunk - 1];
                remaining -= chunk;
                if (remaining == 0) break;
                cluster = fat_read_next_cluster(&info, cluster);
            }
            if (ent.size == 0 || last_byte != '\n') {
                printf("\n");
            }
            return;
        }
        if (argc >= 3 && strcmp(args[1], "read") == 0) {
            uint32_t lba = (uint32_t)strtoul(args[2], NULL, 0);
            uint32_t count = (argc >= 4) ? (uint32_t)strtoul(args[3], NULL, 0) : 1;
            uint32_t total;
            if (count == 0 || count > 8) {
                printf("usb: count must be 1..8\n");
                return;
            }
            if (usb_read_block_sys(lba, g_usb_read_buf, count) < 0) {
                printf("usb: read failed at lba=%u count=%u\n", lba, count);
            } else {
                total = count * 512U;
                for (uint32_t i = 0; i < total; i += 16) {
                    printf("%08x: ", i);
                    for (int j = 0; j < 16; j++) {
                        printf("%02x ", g_usb_read_buf[i + j]);
                    }
                    printf(" ");
                    for (int j = 0; j < 16; j++) {
                        unsigned char c = g_usb_read_buf[i + j];
                        printf("%c", (c >= 32 && c <= 126) ? c : '.');
                    }
                    printf("\n");
                }
            }
            return;
        }
        if (argc >= 5 && strcmp(args[1], "soak") == 0) {
            uint32_t lba = (uint32_t)strtoul(args[2], NULL, 0);
            uint32_t count = (uint32_t)strtoul(args[3], NULL, 0);
            uint32_t iters = (uint32_t)strtoul(args[4], NULL, 0);
            uint32_t ok = 0;
            uint32_t fail = 0;
            if (count == 0 || count > 8 || iters == 0) {
                printf("usb soak: count must be 1..8 and iters > 0\n");
                return;
            }
            for (uint32_t i = 0; i < iters; i++) {
                if (usb_read_block_sys(lba, g_usb_read_buf, count) == 0) ok++;
                else fail++;
            }
            printf("usb soak: lba=%lu count=%lu iters=%lu ok=%lu fail=%lu\n",
                   (unsigned long)lba,
                   (unsigned long)count,
                   (unsigned long)iters,
                   (unsigned long)ok,
                   (unsigned long)fail);
            return;
        }
        int ready = usb_info();
        printf("usb: xhci %s\n", (ready > 0) ? "ready" : "not-ready");
#else
        printf("usb: disabled in this build\n");
#endif
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        exit(run_command_child(argc, args));
    } else if (pid > 0) {
        int status;
        (void)setpgid(pid, pid);
        (void)tcsetpgrp(0, pid);
        waitpid(pid, &status, 0);
        shell_restore_foreground();
    } else {
        perror("fork");
    }
}

static void try_run_bootcmd(void) {
    char buf[512];
    int fd;
    ssize_t n;
    fd = open("/etc/bootcmd", O_RDONLY);
    if (fd < 0) {
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    char* line = buf;
    while (*line) {
        char* next = line;
        while (*next && *next != '\n' && *next != '\r') next++;
        if (*next) {
            *next = '\0';
            next++;
            while (*next == '\n' || *next == '\r') next++;
        }
        if (*line) {
            printf("# bootcmd: %s\n", line);
            run_shell_line(line);
        }
        line = next;
    }
}

static void shell_init_job_control(void) {
    pid_t pgrp;
    if (setpgid(0, 0) == 0 || getpgrp() > 0) {
        pgrp = getpgrp();
        if (pgrp > 0) {
            g_shell_pgrp = pgrp;
            (void)tcsetpgrp(0, pgrp);
        }
    }
}

static void shell_restore_foreground(void) {
    if (g_shell_pgrp > 0) {
        (void)tcsetpgrp(0, g_shell_pgrp);
    }
}

static int run_pipeline_or_redirect(int argc, char** args) {
    int pipe_idx = -1;
    int redir_idx = -1;
    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "|") == 0 && pipe_idx < 0) pipe_idx = i;
        if (strcmp(args[i], ">") == 0 && redir_idx < 0) redir_idx = i;
    }
    if (pipe_idx < 0 && redir_idx < 0) return -2;

    if (pipe_idx >= 0) {
        int pipefd[2];
        pid_t job_pgrp = -1;
        if (pipe_idx == 0 || pipe_idx == argc - 1) {
            printf("sh: invalid pipe syntax\n");
            return 1;
        }
        if (pipe(pipefd) < 0) {
            perror("pipe");
            return 1;
        }
        args[pipe_idx] = NULL;
        pid_t left = fork();
        if (left == 0) {
            (void)setpgid(0, 0);
            close(pipefd[0]);
            dup2(pipefd[1], 1);
            close(pipefd[1]);
            exit(run_command_child(pipe_idx, args));
        }
        if (left > 0) {
            job_pgrp = left;
            (void)setpgid(left, left);
        }
        pid_t right = fork();
        if (right == 0) {
            if (job_pgrp > 0) (void)setpgid(0, job_pgrp);
            close(pipefd[1]);
            dup2(pipefd[0], 0);
            close(pipefd[0]);
            exit(run_command_child(argc - pipe_idx - 1, &args[pipe_idx + 1]));
        }
        if (right > 0 && job_pgrp > 0) {
            (void)setpgid(right, job_pgrp);
            (void)tcsetpgrp(0, job_pgrp);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        int status;
        waitpid(left, &status, 0);
        waitpid(right, &status, 0);
        shell_restore_foreground();
        return 0;
    }

    if (redir_idx == 0 || redir_idx >= argc - 1) {
        printf("sh: invalid redirection syntax\n");
        return 1;
    }
    args[redir_idx] = NULL;
    char out_path[256];
    resolve_shell_path(args[redir_idx + 1], out_path, sizeof(out_path));
    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        int fd = open(out_path, O_CREAT | O_TRUNC | O_RDWR, 0);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
        exit(run_command_child(redir_idx, args));
    }
    if (pid > 0) {
        int status;
        (void)setpgid(pid, pid);
        (void)tcsetpgrp(0, pid);
        waitpid(pid, &status, 0);
        shell_restore_foreground();
        return 0;
    }
    perror("fork");
    return 1;
}

static void run_write_check(const char* path) {
    const char* target = path ? path : "scratch.txt";
    const char* payload = "OrthOS RAMFS write test\n";
    char buf[128];
    int fd = open(target, O_CREAT | O_TRUNC | O_RDWR, 0);
    int n;
    if (fd < 0) {
        printf("writecheck: FAIL open %s\n", target);
        return;
    }
    if (write(fd, payload, (int)strlen(payload)) != (int)strlen(payload)) {
        close(fd);
        printf("writecheck: FAIL write %s\n", target);
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        printf("writecheck: FAIL lseek %s\n", target);
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        printf("writecheck: FAIL readback %s\n", target);
        return;
    }
    buf[n] = '\0';
    if (strcmp(buf, payload) != 0) {
        printf("writecheck: FAIL content %s\n", target);
        return;
    }
    printf("writecheck: PASS %s\n", target);
}

static void run_dir_check(const char* path) {
    const char* target = path ? path : "/";
    DIR* dir = opendir(target);
    struct dirent* ent;
    int count = 0;
    if (!dir) {
        printf("dircheck: FAIL opendir %s\n", target);
        return;
    }
    while ((ent = readdir(dir)) != 0) {
        printf("%s%s\n", ent->d_name, (ent->d_type == DT_DIR) ? "/" : "");
        count++;
        if (count >= 32) break;
    }
    closedir(dir);
    printf("dircheck: PASS %s entries=%d\n", target, count);
}

static void run_scandir_check(const char* path) {
    const char* target = path ? path : "/";
    struct dirent** namelist = 0;
    int n = scandir(target, &namelist, 0, 0);
    if (n < 0) {
        printf("scandircheck: FAIL %s\n", target);
        return;
    }
    for (int i = 0; i < n; i++) {
        printf("%s\n", namelist[i]->d_name);
        free(namelist[i]);
    }
    free(namelist);
    printf("scandircheck: PASS %s entries=%d\n", target, n);
}

static void run_glob_check(const char* pattern) {
    const char* target = pattern ? pattern : "/usb/*";
    glob_t g;
    int ret;
    memset(&g, 0, sizeof(g));
    ret = glob(target, GLOB_MARK, 0, &g);
    if (ret != 0) {
        printf("globcheck: FAIL %s ret=%d\n", target, ret);
        globfree(&g);
        return;
    }
    for (int i = 0; i < g.gl_pathc; i++) {
        printf("%s\n", g.gl_pathv[i]);
    }
    printf("globcheck: PASS %s matches=%d\n", target, g.gl_pathc);
    globfree(&g);
}

static void run_musl_check(void) {
    int pid;
    int status = 0;
    char* argv[] = {
        "ash",
        "-c",
        "pwd && ls /bin && touch /muslcheck.tmp && mkdir /muslcheck.dir && cp /hello.txt /muslcheck.dir/hello.copy && tail -n 1 /hello.txt && wc /hello.txt && stat /muslcheck.dir/hello.copy && rm -f /muslcheck.dir/hello.copy && rmdir /muslcheck.dir && rm -f /muslcheck.tmp",
        NULL
    };

    pid = fork();
    if (pid < 0) {
        printf("muslcheck: FAIL fork\n");
        return;
    }
    if (pid == 0) {
        execve("/bin/ash", argv, environ);
        perror("execve");
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        printf("muslcheck: FAIL waitpid\n");
        return;
    }
    if ((status & 0x7F) != 0 || ((status >> 8) & 0xFF) != 0) {
        printf("muslcheck: FAIL status=%d\n", status);
        return;
    }
    printf("muslcheck: PASS\n");
}

static void try_autostart_saba(void) {
    struct stat st;
    struct stat marker;
    char *argv[] = { "saba", NULL };

    if (stat("/bin/.autostart_saba", &marker) < 0) {
        return;
    }
    if (stat("/bin/saba", &st) < 0) {
        return;
    }
    printf("Autostarting /bin/saba...\n");
    execve("/bin/saba", argv, environ);
    perror("execve /bin/saba");
}

int main(int argc, char** argv) {
    if (argc >= 3 && (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "-ec") == 0 || strcmp(argv[1], "-ce") == 0)) {
        char* line = strdup(argv[2]);
        int rc;
        if (!line) {
            perror("strdup");
            return 1;
        }
        shell_debug_exec_args(argc, argv);
        shell_setup_environment();
        rc = run_shell_command_once(line);
        free(line);
        return rc;
    }

    printf("Welcome to Orthox-64 Shell!\n");
    // 初期シェルとして環境変数をセットアップ
    shell_setup_environment();
    try_run_bootcmd();
    // try_autostart_saba();

    while (1) {
        char prompt[MAX_LINE + 8];
        sync_shell_cwd();
        snprintf(prompt, sizeof(prompt), "%s$ ", g_cwd);
        printf("%s", prompt);
        fflush(stdout);

        char line[MAX_LINE];
        get_line(prompt, line, sizeof(line));

        if (strlen(line) == 0) continue;
        run_shell_line(line);
    }

    printf("Goodbye!\n");
    return 0;
}
