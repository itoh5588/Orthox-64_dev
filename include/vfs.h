#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_PATH_MAX 32
#define VFS_MAX_MOUNTPOINTS 16

enum vfs_mount_kind {
    VFS_MOUNT_NONE = 0,
    VFS_MOUNT_USB_FAT = 1,
    VFS_MOUNT_IMAGE_FS = 2,
};

struct vfs_mountpoint {
    char path[VFS_PATH_MAX];
    uint32_t kind;
    void* opaque;
    int active;
};

void vfs_init(void);
int vfs_register_mountpoint(const char* path, uint32_t kind, void* opaque);
const struct vfs_mountpoint* vfs_find_mountpoint(const char* path, const char** subpath);
size_t vfs_list_mountpoints(const struct vfs_mountpoint** out, size_t max_entries);

#endif
