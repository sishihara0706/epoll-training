import socket
import time
import json

s = socket.socket()
s.connect(("127.0.0.1", 8080))

for i in range(100000):
    data = {
        "id": i,
        "device": "sensor-01",
        "temperature": 25.5,
        "pressure": 101.3,
        "status": "RUNNING"
    }

    msg = json.dumps(data).encode("utf-8") + b"\n"

    s.sendall(msg)

print("finished sending")

# 応答を読まない
time.sleep(60)
s.close()
