#!/bin/bash
make all
(
sleep 10
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
echo "sendkey k"
sleep 0.2
echo "sendkey e"
sleep 0.2
echo "sendkey y"
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
sleep 1
echo "sendkey a"
sleep 0.2
echo "sendkey b"
sleep 0.2
echo "sendkey c"
sleep 0.2
echo "sendkey ret"
sleep 8
) | qemu-system-x86_64 -M pc -cpu max -m 2G -cdrom orthos.iso -boot d -display none -serial file:serial_output.log -monitor stdio -no-reboot
