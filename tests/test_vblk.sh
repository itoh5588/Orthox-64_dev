#!/bin/bash
(
echo "vblk_test"
sleep 2
echo "poweroff"
sleep 1
) | qemu-system-x86_64 -machine pc -cpu max -m 2G -cdrom orthos.iso -boot d -display none -device virtio-blk-pci,drive=hd0,disable-modern=on -drive id=hd0,file=rootfs.img,format=raw,if=none -serial stdio > test_out1.log 2>&1

(
echo "vblk_test read"
sleep 2
echo "poweroff"
sleep 1
) | qemu-system-x86_64 -machine pc -cpu max -m 2G -cdrom orthos.iso -boot d -display none -device virtio-blk-pci,drive=hd0,disable-modern=on -drive id=hd0,file=rootfs.img,format=raw,if=none -serial stdio > test_out2.log 2>&1
