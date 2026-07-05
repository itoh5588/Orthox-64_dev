export PATH=/bin:/usr/bin:/
echo native-kernel-build-start
mkdir -p /kbuild/kernel /kbuild/lwip/core/ipv4 /kbuild/lwip/netif
make -f /src/kernel-build/Makefile BUILD=/kbuild OUTPUT=/kernel.elf
echo native-kernel-build-end
