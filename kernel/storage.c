#include "storage.h"

extern void* memcpy(void* dest, const void* src, size_t n);

struct memory_storage_device {
    uint8_t* base;
};

static struct storage_device g_storage_devices[STORAGE_MAX_DEVICES];
static struct memory_storage_device g_memory_storage[STORAGE_MAX_DEVICES];

static int strcmp_exact_storage(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void copy_storage_name(char* dst, const char* src, size_t size) {
    size_t i;
    if (!dst || size == 0) return;
    for (i = 0; i + 1 < size && src && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int memory_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    struct memory_storage_device* mem = (struct memory_storage_device*)ctx;
    if (!mem || !mem->base || !buf) return -1;
    memcpy(buf, mem->base + lba * 512U, count * 512U);
    return 0;
}

static int memory_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    struct memory_storage_device* mem = (struct memory_storage_device*)ctx;
    if (!mem || !mem->base || !buf) return -1;
    memcpy(mem->base + lba * 512U, buf, count * 512U);
    return 0;
}

void storage_init(void) {
    size_t i;
    for (i = 0; i < STORAGE_MAX_DEVICES; i++) {
        g_storage_devices[i].name[0] = '\0';
        g_storage_devices[i].block_size = 0;
        g_storage_devices[i].block_count = 0;
        g_storage_devices[i].read = 0;
        g_storage_devices[i].write = 0;
        g_storage_devices[i].ctx = 0;
        g_storage_devices[i].read_only = 0;
        g_storage_devices[i].in_use = 0;
        g_memory_storage[i].base = 0;
    }
}

int storage_register_device(const char* name, uint32_t block_size, uint64_t block_count,
                            storage_read_fn read, storage_write_fn write, void* ctx, int read_only) {
    size_t i;
    if (!name || !name[0] || block_size == 0 || !read) return -1;
    for (i = 0; i < STORAGE_MAX_DEVICES; i++) {
        if (!g_storage_devices[i].in_use) {
            copy_storage_name(g_storage_devices[i].name, name, sizeof(g_storage_devices[i].name));
            g_storage_devices[i].block_size = block_size;
            g_storage_devices[i].block_count = block_count;
            g_storage_devices[i].read = read;
            g_storage_devices[i].write = write;
            g_storage_devices[i].ctx = ctx;
            g_storage_devices[i].read_only = read_only ? 1 : 0;
            g_storage_devices[i].in_use = 1;
            return 0;
        }
    }
    return -1;
}

struct storage_device* storage_find_device(const char* name) {
    size_t i;
    if (!name) return 0;
    for (i = 0; i < STORAGE_MAX_DEVICES; i++) {
        if (g_storage_devices[i].in_use && strcmp_exact_storage(g_storage_devices[i].name, name)) {
            return &g_storage_devices[i];
        }
    }
    return 0;
}

int storage_read_blocks(const char* name, uint64_t lba, void* buf, size_t count) {
    struct storage_device* dev = storage_find_device(name);
    if (!dev || !buf || count == 0) return -1;
    if (lba + count > dev->block_count) return -1;
    return dev->read(dev->ctx, lba, buf, count);
}

int storage_write_blocks(const char* name, uint64_t lba, const void* buf, size_t count) {
    struct storage_device* dev = storage_find_device(name);
    if (!dev || !buf || count == 0 || dev->read_only || !dev->write) return -1;
    if (lba + count > dev->block_count) return -1;
    return dev->write(dev->ctx, lba, buf, count);
}

int storage_register_memory_device(const char* name, void* base, uint32_t block_size,
                                   uint64_t block_count, int read_only) {
    size_t i;
    if (!base || block_size != 512U) return -1;
    for (i = 0; i < STORAGE_MAX_DEVICES; i++) {
        if (!g_storage_devices[i].in_use) {
            g_memory_storage[i].base = (uint8_t*)base;
            return storage_register_device(name, block_size, block_count,
                                           memory_storage_read,
                                           read_only ? 0 : memory_storage_write,
                                           &g_memory_storage[i], read_only);
        }
    }
    return -1;
}
