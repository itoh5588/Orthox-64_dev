#!/bin/bash
make all
(
sleep 8
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
echo "sendkey t"
sleep 0.2
echo "sendkey i"
sleep 0.2
echo "sendkey m"
sleep 0.2
echo "sendkey e"
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
sleep 6
) | qemu-system-x86_64 -M pc -cpu max -m 2G -cdrom orthos.iso -boot d -display none -serial file:serial_output.log -monitor stdio -no-reboot
