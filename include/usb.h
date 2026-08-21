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
int usb_hid_keyboard_ready(void);
int usb_hid_keyboard_poll(uint8_t report[8]);

#endif
