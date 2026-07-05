#!/bin/bash
(
sleep 3
echo "sendkey h"
sleep 0.1
echo "sendkey e"
sleep 0.1
echo "sendkey l"
sleep 0.1
echo "sendkey l"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey ret"
sleep 2
) | qemu-system-x86_64 -M pc -m 2G -cdrom orthos.iso -boot d -display none -serial file:serial.log -monitor stdio
