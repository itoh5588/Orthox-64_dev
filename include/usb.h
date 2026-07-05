#ifndef USB_H
#define USB_H

void usb_init(void);
void usb_dump_status(void);
int usb_is_ready(void);
int usb_mass_storage_ready(void);
int usb_block_device_ready(void);
int usb_read_block(uint32_t lba, void* buf, uint32_t count);

#endif
