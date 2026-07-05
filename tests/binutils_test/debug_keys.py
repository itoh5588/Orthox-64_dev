import socket, time, sys

def send_qemu(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect("qemu.sock")
        s.sendall(cmd.encode('utf-8') + b'\n')
        s.close()
    except Exception as e:
        pass

# テスト: いろいろなキーを送ってみる
time.sleep(2)
for k in ["a", "spc", "b", "space", "c", "dot", "d", "kp_decimal", "e", "minus", "f", "slash", "g"]:
    send_qemu(f"sendkey {k}")
    time.sleep(0.5)
send_qemu("sendkey ret")
