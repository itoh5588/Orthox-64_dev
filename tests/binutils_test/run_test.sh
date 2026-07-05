#!/bin/bash
rm -f qemu.sock serial.log
qemu-system-x86_64 -M pc -m 2G -cdrom orthos.iso -boot d -display none \
    -serial file:serial.log -monitor unix:qemu.sock,server,nowait -k en-us &
QEMU_PID=$!

echo "Waiting for Orthox-64 Shell..."
for i in {1..30}; do
    if grep -q "Welcome to Orthox-64 Shell!" serial.log 2>/dev/null; then
        echo "Shell started!"
        break
    fi
    sleep 1
done

sleep 2
# send_string.py を改善したものを使用
python3 << 'EOF'
import socket, time, sys

def send_qemu(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect("qemu.sock")
        s.sendall(cmd.encode('utf-8') + b'\n')
        s.close()
    except:
        pass

def send_key(k):
    send_qemu(f"sendkey {k}")
    time.sleep(0.2)

# as test.s -o test.o
for c in "as test.s -o test.o":
    k = c
    if c == ' ': k = 'spc'
    if c == '.': k = 'dot'
    if c == '-': k = 'minus'
    send_key(k)
send_key('ret')
time.sleep(5)

# ld test.o -o test
for c in "ld test.o -o test":
    k = c
    if c == ' ': k = 'spc'
    if c == '.': k = 'dot'
    if c == '-': k = 'minus'
    send_key(k)
send_key('ret')
time.sleep(5)

# ./test
for c in "./test":
    k = c
    if c == '.': k = 'dot'
    if c == '/': k = 'slash'
    send_key(k)
send_key('ret')
time.sleep(2)
EOF

echo "--- Serial Output ---"
cat serial.log
echo "---------------------"

kill $QEMU_PID
wait $QEMU_PID 2>/dev/null
rm -f qemu.sock
