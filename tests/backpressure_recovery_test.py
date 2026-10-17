
import socket
import threading
import time
import json

HOST = "127.0.0.1"
PORT = 8080

MESSAGES = 5000
PAYLOAD_SIZE = 900
NO_READ_SECONDS = 3
IO_TIMEOUT = 120


def make_message(i):
    data = {
        "client_id": 999,
        "id": i,
        "payload": "A" * PAYLOAD_SIZE
    }
    return (json.dumps(data) + "\n").encode("utf-8")


def main():
    expected = [make_message(i) for i in range(MESSAGES)]

    sent = 0
    received = 0
    send_error = []
    buffer = bytearray()

    with socket.create_connection((HOST, PORT), timeout=5) as s:
        # 受信ウィンドウを小さくして、
        # サーバー側で詰まりやすくする
        s.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_RCVBUF,
            16384
        )
        s.settimeout(IO_TIMEOUT)

        def sender():
            nonlocal sent

            try:
                for msg in expected:
                    s.sendall(msg)
                    sent += 1
            except Exception as e:
                send_error.append(e)

        thread = threading.Thread(target=sender, daemon=True)
        thread.start()

        print("[Phase 1] Sending without recv()...")
        time.sleep(NO_READ_SECONDS)

        print(f"Sent so far: {sent}/{MESSAGES}")
        print(f"Sender alive: {thread.is_alive()}")

        print("[Phase 2] Resuming recv()...")
        start = time.perf_counter()

        while received < MESSAGES:
            chunk = s.recv(65536)

            if not chunk:
                raise ConnectionError(
                    f"Connection closed at {received}/{MESSAGES}"
                )

            buffer.extend(chunk)

            while b"\n" in buffer:
                line, _, rest = buffer.partition(b"\n")
                buffer = bytearray(rest)

                if received >= MESSAGES:
                    raise AssertionError("Extra message")

                actual = bytes(line) + b"\n"

                if actual != expected[received]:
                    raise AssertionError(
                        f"Mismatch at message {received}"
                    )

                received += 1

                if received % 1000 == 0:
                    print(f"Received: {received}/{MESSAGES}")

        elapsed = time.perf_counter() - start

        # 受信完了後、送信スレッドの終了も確認
        thread.join(timeout=5)

        if thread.is_alive():
            raise RuntimeError("Sender did not finish")

        if send_error:
            raise RuntimeError(f"Sender error: {send_error[0]}")

        if sent != MESSAGES:
            raise AssertionError(
                f"Sent {sent}/{MESSAGES}"
            )

        if buffer:
            raise AssertionError("Unexpected trailing data")

        print("\n===== TEST RESULT =====")
        print(f"Sent:     {sent}")
        print(f"Received: {received}")
        print(f"Recovery time: {elapsed:.2f} sec")
        print("TEST PASSED")


if __name__ == "__main__":
    main()

