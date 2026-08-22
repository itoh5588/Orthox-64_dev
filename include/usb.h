#ifndef USB_H
#define USB_H

void usb_init(void);
void usb_dump_status(void);
int usb_is_ready(void);
int usb_mass_storage_ready(void);
int usb_block_device_ready(void);
int usb_read_block(uint32_t lba, void* buf, uint32_t count);

/* HID キーボード (boot protocol)。レポートは 8 バイト固定:
 *   [0] 修飾キー  [1] 予約  [2..7] 同時に押されているキーのコード */
int usb_hid_keyboard_init(void);
/* **ハブの抜き差しを見る。**制御転送を投げるので割り込みからは呼べない。
 * task_idle_loop() から 500ms に 1 回だけ実際の確認をする (B-1) */
void usb_hotplug_poll(void);

/* **xHCI の割り込み入口 (A-1)。**アーキ側の IRQ ハンドラから呼ぶ。
 * イベントリングを置き場へ吸い出すだけで、制御転送は投げない */
void usb_xhci_irq(void);

/* **ホスト側の割り込みの口を開ける。**usb.c から呼ばれる。
 * aarch64 は GIC の INTID を開ける (kernel/aarch64/boot.c)。
 * 実装が無いアーキでは弱いシンボルの空実装が使われ、動きはポーリングのまま */
void usb_arch_irq_enable(void);

/* 待っているあいだだけ割り込みを開ける (A-1b)。**idle は割り込みを閉じて
 * 走っている**ので、開けないと制御転送のあいだ xHCI の割り込みが届かない。
 * 実装しないアーキでは何もしない = いままでのポーリングのまま。
 * begin が返した値をそのまま end に渡す */
uint64_t usb_arch_irq_window_begin(void);
void usb_arch_irq_window_end(uint64_t token);
int usb_hid_keyboard_ready(void);
int usb_hid_keyboard_poll(uint8_t report[8]);

#endif
