/*
 * Minimal M-mode firmware for the Arty S7-50 MicroBlaze V SoC.
 * Runs from boot BRAM at 0x0, sets up delegation, and enters the
 * S-mode kernel at 0x80200000 with a0 = hartid, a1 = DTB address.
 */
        .section .text.start
        .globl  _start
_start:
        la      sp, fw_stack_top
        csrw    mscratch, sp
        la      t0, fw_trap
        csrw    mtvec, t0
        call    fw_main                 /* returns the kernel entry in a0 */
        mv      t0, a0
        csrr    a0, mhartid
        la      a1, fw_dtb
        jr      t0                      /* not reached: fw_main does mret */

/*
 * Trap entry. mscratch holds the M-mode stack top while S/U code runs.
 * The frame is 32 x 8 bytes; slot n holds xn (slot 0 unused).
 */
        .balign 4
        .globl  fw_trap
fw_trap:
        csrrw   sp, mscratch, sp
        addi    sp, sp, -256
        sd      x1, 8(sp)
        sd      x3, 24(sp)
        sd      x4, 32(sp)
        sd      x5, 40(sp)
        sd      x6, 48(sp)
        sd      x7, 56(sp)
        sd      x8, 64(sp)
        sd      x9, 72(sp)
        sd      x10, 80(sp)
        sd      x11, 88(sp)
        sd      x12, 96(sp)
        sd      x13, 104(sp)
        sd      x14, 112(sp)
        sd      x15, 120(sp)
        sd      x16, 128(sp)
        sd      x17, 136(sp)
        sd      x18, 144(sp)
        sd      x19, 152(sp)
        sd      x20, 160(sp)
        sd      x21, 168(sp)
        sd      x22, 176(sp)
        sd      x23, 184(sp)
        sd      x24, 192(sp)
        sd      x25, 200(sp)
        sd      x26, 208(sp)
        sd      x27, 216(sp)
        sd      x28, 224(sp)
        sd      x29, 232(sp)
        sd      x30, 240(sp)
        sd      x31, 248(sp)
        csrr    t0, mscratch            /* interrupted sp */
        sd      t0, 16(sp)
        addi    t0, sp, 256
        csrw    mscratch, t0            /* restore M stack top */
        mv      a0, sp
        call    fw_trap_handler
        ld      x1, 8(sp)
        ld      x3, 24(sp)
        ld      x4, 32(sp)
        ld      x5, 40(sp)
        ld      x6, 48(sp)
        ld      x7, 56(sp)
        ld      x8, 64(sp)
        ld      x9, 72(sp)
        ld      x10, 80(sp)
        ld      x11, 88(sp)
        ld      x12, 96(sp)
        ld      x13, 104(sp)
        ld      x14, 112(sp)
        ld      x15, 120(sp)
        ld      x16, 128(sp)
        ld      x17, 136(sp)
        ld      x18, 144(sp)
        ld      x19, 152(sp)
        ld      x20, 160(sp)
        ld      x21, 168(sp)
        ld      x22, 176(sp)
        ld      x23, 184(sp)
        ld      x24, 192(sp)
        ld      x25, 200(sp)
        ld      x26, 208(sp)
        ld      x27, 216(sp)
        ld      x28, 224(sp)
        ld      x29, 232(sp)
        ld      x30, 240(sp)
        ld      x31, 248(sp)
        ld      sp, 16(sp)
        mret

        .section .rodata
        .balign 8
        .globl  fw_dtb
fw_dtb:
        .incbin DTB_PATH
        .globl  fw_dtb_end
fw_dtb_end:

        .section .bss
        .balign 16
        .space  8192
fw_stack_top:
