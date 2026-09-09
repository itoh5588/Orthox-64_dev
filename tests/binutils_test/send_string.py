import socket, time, sys

def send_qemu(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect("qemu.sock")
        # Receive the prompt (optional)
        # s.recv(1024)
        s.sendall(cmd.encode('utf-8') + b'\n')
        s.close()
    except Exception as e:
        pass

def send_string(s):
    for char in s:
        key = char
        if char == ' ': key = 'spc'
        if char == '.': key = 'dot'
        if char == '-': key = 'minus'
        if char == '/': key = 'slash'
        if char == '_': key = 'shift-minus'
        send_qemu(f"sendkey {key}")
        time.sleep(0.1)
    send_qemu("sendkey ret")

time.sleep(1) # wait for QEMU monitor to be ready
if len(sys.argv) > 1:
    send_string(sys.argv[1])
