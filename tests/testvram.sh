#!/bin/bash
make all
(
sleep 8
# /boot/testvram.elf と入力
echo "sendkey slash"
sleep 0.2
echo "sendkey b"
sleep 0.2
echo "sendkey o"
sleep 0.2
echo "sendkey o"
sleep 0.2
echo "sendkey t"
sleep 0.2
echo "sendkey slash"
sleep 0.2
echo "sendkey t"
sleep 0.2
echo "sendkey e"
sleep 0.2
echo "sendkey s"
sleep 0.2
echo "sendkey t"
sleep 0.2
echo "sendkey v"
sleep 0.2
echo "sendkey r"
sleep 0.2
echo "sendkey a"
sleep 0.2
echo "sendkey m"
sleep 0.2
echo "sendkey dot"
sleep 0.2
echo "sendkey e"
sleep 0.2
echo "sendkey l"
sleep 0.2
echo "sendkey f"
sleep 0.2
echo "sendkey ret"
sleep 5
) | qemu-system-x86_64 -M pc -cpu max -m 2G -cdrom orthos.iso -boot d -display none -serial file:serial_output.log -monitor stdio -no-reboot
