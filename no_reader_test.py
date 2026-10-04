import socket
import time

s = socket.socket()
s.connect(("127.0.0.1", 8080))

msg = b"A" * 1000 + b"\n"

for i in range(100000):
    s.sendall(msg)

print("finished sending")

# 重要: recv()しない
time.sleep(60)
