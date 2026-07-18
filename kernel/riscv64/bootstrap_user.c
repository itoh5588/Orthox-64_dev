#include <stddef.h>
#include <stdint.h>
#include "elf64.h"
#include "riscv64/bootstrap_user.h"

struct riscv64_bootstrap_user_elf_image {
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;
    uint8_t code[102];
};

static const struct riscv64_bootstrap_user_elf_image g_riscv64_bootstrap_user_elf = {
    .ehdr = {
        .e_ident = { 0x7f, 'E', 'L', 'F', ELFCLASS64, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        .e_type = 2,
        .e_machine = 243,
        .e_version = 1,
        .e_entry = 0x0000000000400000ULL,
        .e_phoff = sizeof(Elf64_Ehdr),
        .e_shoff = 0,
        .e_flags = 0,
        .e_ehsize = sizeof(Elf64_Ehdr),
        .e_phentsize = sizeof(Elf64_Phdr),
        .e_phnum = 1,
        .e_shentsize = 0,
        .e_shnum = 0,
        .e_shstrndx = 0,
    },
    .phdr = {
        .p_type = PT_LOAD,
        .p_flags = PF_R | PF_X,
        .p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr),
        .p_vaddr = 0x0000000000400000ULL,
        .p_paddr = 0,
        .p_filesz = 102,
        .p_memsz = 4096,
        .p_align = 4096,
    },
    .code = {
        0x13, 0x05, 0x01, 0xff,
        0x93, 0x05, 0x00, 0x01,
        0x93, 0x08, 0xf0, 0x04,
        0x73, 0x00, 0x00, 0x00,
        0x13, 0x05, 0x10, 0x00,
        0x97, 0x05, 0x00, 0x00,
        0x93, 0x85, 0x05, 0x05,
        0x13, 0x06, 0x10, 0x00,
        0x93, 0x08, 0x10, 0x00,
        0x73, 0x00, 0x00, 0x00,
        0x13, 0x05, 0x10, 0x00,
        0x93, 0x05, 0x01, 0xff,
        0x13, 0x06, 0x10, 0x00,
        0x93, 0x08, 0x10, 0x00,
        0x73, 0x00, 0x00, 0x00,
        0x13, 0x05, 0x10, 0x00,
        0x97, 0x05, 0x00, 0x00,
        0x93, 0x85, 0x55, 0x02,
        0x13, 0x06, 0x10, 0x00,
        0x93, 0x08, 0x10, 0x00,
        0x73, 0x00, 0x00, 0x00,
        0x13, 0x05, 0x00, 0x00,
        0x93, 0x08, 0xc0, 0x03,
        0x73, 0x00, 0x00, 0x00,
        0x6f, 0x00, 0x00, 0x00,
        0x43, 0x0a,
    },
};

static int riscv64_bootstrap_user_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int riscv64_bootstrap_user_file_data(const char* path, void** data, size_t* size) {
    if (!riscv64_bootstrap_user_streq(path, "/bootstrap-user")) return -1;
    if (data) *data = (void*)&g_riscv64_bootstrap_user_elf;
    if (size) *size = sizeof(g_riscv64_bootstrap_user_elf);
    return 0;
}
