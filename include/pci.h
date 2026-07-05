#ifndef PCI_H
#define PCI_H

#include <stdint.h>

struct pci_device_info {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq_line;
};

void pci_init(void);
int pci_find_device(struct pci_device_info* out,
                    int vendor_id,
                    int device_id,
                    int class_code,
                    int subclass,
                    int prog_if);
int pci_find_xhci(struct pci_device_info* out);
int pci_find_virtio_net(struct pci_device_info* out);
int pci_find_virtio_blk(struct pci_device_info* out);
int pci_find_virtio_blk_n(int n, struct pci_device_info* out);
uint32_t pci_get_bar_raw(const struct pci_device_info* dev, uint8_t bar_index);
uint64_t pci_get_bar_mmio(const struct pci_device_info* dev, uint8_t bar_index);
uint16_t pci_get_bar_iobase(const struct pci_device_info* dev, uint8_t bar_index);
uint64_t pci_get_bar0_mmio(const struct pci_device_info* dev);
uint16_t pci_get_bar0_iobase(const struct pci_device_info* dev);
void pci_enable_mmio_busmaster(const struct pci_device_info* dev);
void pci_enable_io_busmaster(const struct pci_device_info* dev);
int pci_enable_msi(const struct pci_device_info* dev, uint8_t vector);
int pci_enable_msix(const struct pci_device_info* dev, uint8_t vector, uint16_t entry);

#endif
