set -x
export PATH=/bin:/usr/bin:/
echo musl-busybox-start
echo shell=$0
echo pwd-before
/bin/pwd
echo env-count
/bin/env | /bin/wc -l
echo path-before
/bin/printenv PATH
echo ls-bin
/bin/ls /bin
echo cat-hello
/bin/cat /hello.txt
echo head-hello
/bin/head -n 1 /hello.txt
echo tail-hello
/bin/tail -n 1 /hello.txt
echo stat-hello
/bin/stat /hello.txt
echo touch-bbx
/bin/touch /bbx.txt
echo ls-root
/bin/ls /
echo rm-bbx
/bin/rm /bbx.txt
echo attest
/bin/at_test.elf
echo musl-busybox-ok
