#include <stdint.h>
#include "syscall.h"
#include "task.h"

void puts(const char *s);
extern struct task* task_list;

#define KB_PORT 0x60
#define KB_BUF_SIZE 256

static uint8_t kb_buf[KB_BUF_SIZE];
static int kb_head = 0;
static int kb_tail = 0;

static struct key_event key_events[KB_BUF_SIZE];
static int ev_head = 0;
static int ev_tail = 0;
static struct task* kb_waiter = 0;
static uint8_t kb_shift_down = 0;
static uint8_t kb_ctrl_down = 0;
static uint8_t kb_alt_down = 0;
static uint8_t kb_caps_lock = 0;
static uint8_t kb_e0_prefix = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %w1, %b0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// 簡易JISキーボードマップ (シフトなし、スキャンコードセット1)
static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','^','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','@','[','\n',
    0,  'a','s','d','f','g','h','j','k','l',';',':',  0,  0, ']',
    'z','x','c','v','b','n','m',',','.','/',  0, '*',
    0, ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0, '\\', 0,  0,  0,  0,  0, '\\', 0,  0,  0,  0,  0,  0
};

static const char keymap_shift[128] = {
    0,  27, '!','"','#','$','%','&','\'','(',')','~','=','~','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','`','{','\n',
    0,  'A','S','D','F','G','H','J','K','L','+','*',  0,  0, '}',
    'Z','X','C','V','B','N','M','<','>','?',  0, '*',
    0, ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0, '_', 0,  0,  0,  0,  0, '|', 0,  0,  0,  0,  0,  0
};

static char keyboard_ascii_from_scancode(uint8_t code, uint8_t extended) {
    char c = 0;

    if (extended) {
        switch (code) {
            case 28: return '\n';   /* keypad enter */
            case 53: return '/';    /* keypad slash */
            case 71: return '\033'; /* home */
            case 72: return 0;      /* up */
            case 73: return 0;      /* page up */
            case 75: return 0;      /* left */
            case 77: return 0;      /* right */
            case 79: return 0;      /* end */
            case 80: return 0;      /* down */
            case 81: return 0;      /* page down */
            case 82: return 0;      /* insert */
            case 83: return 0x7f;   /* delete */
            default: return 0;
        }
    }

    c = kb_shift_down ? keymap_shift[code] : keymap[code];
    if (c >= 'a' && c <= 'z' && kb_caps_lock) {
        c = kb_shift_down ? c : (char)(c - 'a' + 'A');
    } else if (c >= 'A' && c <= 'Z' && kb_caps_lock) {
        c = kb_shift_down ? (char)(c - 'A' + 'a') : c;
    }
    if (kb_ctrl_down) {
        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 1);
    }
    return c;
}

static void send_sigint_to_foreground_pgrp(void) {
    struct task* t = task_list;
    extern int tty_get_foreground_pgrp(void);
    int fg = tty_get_foreground_pgrp();
    while (t) {
        if (t->pgid == fg && t->pid != 1) {
            t->sig_pending |= (1ULL << 2);
            task_mark_zombie(t, 130);
        }
        t = t->next;
    }
}

static void wake_kb_waiter(void) {
    if (!kb_waiter) return;
    if (kb_waiter->state == TASK_SLEEPING) {
        task_wake(kb_waiter);
    }
    kb_waiter = 0;
}

void keyboard_handler(void) {
    uint8_t scancode = inb(KB_PORT);
    uint8_t extended = 0;
    if (scancode == 0xE0) {
        kb_e0_prefix = 1;
        return;
    }
    extended = kb_e0_prefix;
    kb_e0_prefix = 0;

    uint8_t pressed = (scancode & 0x80) ? 0 : 1;
    uint8_t code = scancode & 0x7F;

    if (!extended && (code == 42 || code == 54)) {
        kb_shift_down = pressed ? 1 : 0;
    }
    if ((!extended && code == 29) || (extended && code == 29)) {
        kb_ctrl_down = pressed ? 1 : 0;
    }
    if ((!extended && code == 56) || (extended && code == 56)) {
        kb_alt_down = pressed ? 1 : 0;
    }
    if (!extended && code == 58 && pressed) {
        kb_caps_lock ^= 1;
    }

    char c = keyboard_ascii_from_scancode(code, extended);

    int next_ev_head = (ev_head + 1) % KB_BUF_SIZE;
    if (next_ev_head != ev_tail) {
        key_events[ev_head].pressed = pressed;
        key_events[ev_head].scancode = extended ? (uint8_t)(code | 0x80) : code;
        key_events[ev_head].ascii = (uint16_t)(uint8_t)c;
        ev_head = next_ev_head;
    }

    if (pressed && c) {
        if (c == 3) {
            send_sigint_to_foreground_pgrp();
            return;
        }
        int next_head = (kb_head + 1) % KB_BUF_SIZE;
        if (next_head != kb_tail) {
            kb_buf[kb_head] = c;
            kb_head = next_head;
            wake_kb_waiter();
        }
    }
}

void serial_handler(void) {
    uint8_t c = inb(0x3f8);
    if (c == 3) {
        send_sigint_to_foreground_pgrp();
        return;
    }
    if (c == '\r') c = '\n'; // terminal CR to LF

    int next_head = (kb_head + 1) % KB_BUF_SIZE;
    if (next_head != kb_tail) {
        kb_buf[kb_head] = c;
        kb_head = next_head;
        wake_kb_waiter();
    }
}

// ユーザー空間から呼ばれる (sys_read fd=0)
int kb_read(char* buf, int count) {
    int read = 0;
    while (read < count) {
        if (kb_head == kb_tail) break; // バッファ空
        buf[read++] = kb_buf[kb_tail];
        kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    }
    return read;
}

int kb_get_event(struct key_event* ev) {
    if (ev_head == ev_tail) return 0;
    *ev = key_events[ev_tail];
    ev_tail = (ev_tail + 1) % KB_BUF_SIZE;
    return 1;
}

void kb_set_waiter(struct task* t) {
    kb_waiter = t;
}

void kb_clear_waiter(struct task* t) {
    if (kb_waiter == t) kb_waiter = 0;
}
