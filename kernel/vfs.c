#include "vfs.h"

static struct vfs_mountpoint g_vfs_mountpoints[VFS_MAX_MOUNTPOINTS];

static int path_component_match_vfs(const char* path, const char* prefix) {
    while (*prefix) {
        if (*path != *prefix) return 0;
        path++;
        prefix++;
    }
    return (*path == '\0' || *path == '/');
}

static const char* normalize_vfs_path(const char* path) {
    if (!path) return "";
    while (*path == '/') path++;
    if (path[0] == '.' && path[1] == '/') {
        path += 2;
        while (*path == '/') path++;
    }
    return path;
}

static void copy_mount_path(char* dst, const char* src) {
    size_t i;
    if (!dst) return;
    for (i = 0; i + 1 < VFS_PATH_MAX && src && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void vfs_init(void) {
    size_t i;
    for (i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        g_vfs_mountpoints[i].path[0] = '\0';
        g_vfs_mountpoints[i].kind = VFS_MOUNT_NONE;
        g_vfs_mountpoints[i].opaque = 0;
        g_vfs_mountpoints[i].active = 0;
    }
}

int vfs_register_mountpoint(const char* path, uint32_t kind, void* opaque) {
    size_t i;
    const char* norm = normalize_vfs_path(path);
    if (!norm[0]) return -1;
    for (i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        if (!g_vfs_mountpoints[i].active) {
            copy_mount_path(g_vfs_mountpoints[i].path, norm);
            g_vfs_mountpoints[i].kind = kind;
            g_vfs_mountpoints[i].opaque = opaque;
            g_vfs_mountpoints[i].active = 1;
            return 0;
        }
    }
    return -1;
}

const struct vfs_mountpoint* vfs_find_mountpoint(const char* path, const char** subpath) {
    const struct vfs_mountpoint* best = 0;
    const char* norm = normalize_vfs_path(path);
    size_t best_len = 0;
    size_t i;
    for (i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        size_t len = 0;
        if (!g_vfs_mountpoints[i].active) continue;
        while (g_vfs_mountpoints[i].path[len]) len++;
        if (!path_component_match_vfs(norm, g_vfs_mountpoints[i].path)) continue;
        if (len < best_len) continue;
        best = &g_vfs_mountpoints[i];
        best_len = len;
    }
    if (!best) return 0;
    if (subpath) {
        *subpath = norm + best_len;
        if (**subpath == '/') (*subpath)++;
    }
    return best;
}

size_t vfs_list_mountpoints(const struct vfs_mountpoint** out, size_t max_entries) {
    size_t i;
    size_t count = 0;
    for (i = 0; i < VFS_MAX_MOUNTPOINTS; i++) {
        if (!g_vfs_mountpoints[i].active) continue;
        if (out && count < max_entries) out[count] = &g_vfs_mountpoints[i];
        count++;
    }
    return count;
}
