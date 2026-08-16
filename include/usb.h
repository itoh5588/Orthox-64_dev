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
int usb_hid_keyboard_ready(void);
int usb_hid_keyboard_poll(uint8_t report[8]);

#endif
