#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

#define STORAGE_NAME_MAX 16
#define STORAGE_MAX_DEVICES 16

typedef int (*storage_read_fn)(void* ctx, uint64_t lba, void* buf, size_t count);
typedef int (*storage_write_fn)(void* ctx, uint64_t lba, const void* buf, size_t count);

struct storage_device {
    char name[STORAGE_NAME_MAX];
    uint32_t block_size;
    uint64_t block_count;
    storage_read_fn read;
    storage_write_fn write;
    void* ctx;
    int read_only;
    int in_use;
};

void storage_init(void);
int storage_register_device(const char* name, uint32_t block_size, uint64_t block_count,
                            storage_read_fn read, storage_write_fn write, void* ctx, int read_only);
struct storage_device* storage_find_device(const char* name);
int storage_read_blocks(const char* name, uint64_t lba, void* buf, size_t count);
int storage_write_blocks(const char* name, uint64_t lba, const void* buf, size_t count);
int storage_register_memory_device(const char* name, void* base, uint32_t block_size,
                                   uint64_t block_count, int read_only);

#endif
