import socket
import time
import json
import threading

HOST = "127.0.0.1"
PORT = 8080

TOTAL_MESSAGES = 10000
WAIT_SECONDS = 5


def make_message(i):
    data = {
        "id": i,
        "device": "sensor-01",
        "status": "RUNNING",
        "payload": "A" * 900
    }
    return (json.dumps(data) + "\n").encode("utf-8")


def main():
    expected = [
        make_message(i)
        for i in range(TOTAL_MESSAGES)
    ]

    sent = 0
    send_error = []

    with socket.create_connection((HOST, PORT)) as s:
        s.settimeout(30)

        def sender():
            nonlocal sent

            try:
                for msg in expected:
                    s.sendall(msg)
                    sent += 1

                print(f"[SENDER] Finished: {sent} messages")

            except Exception as e:
                send_error.append(e)
                print(f"[SENDER] Error: {e}")

        print("[1] Start sending")

        thread = threading.Thread(target=sender)
        thread.start()

        print(f"[2] No recv for {WAIT_SECONDS} seconds")
        time.sleep(WAIT_SECONDS)

        print("[3] Start receiving")

        received = 0
        buffer = bytearray()

        while received < TOTAL_MESSAGES:
            chunk = s.recv(65536)

            if not chunk:
                raise RuntimeError(
                    f"Connection closed: {received}/{TOTAL_MESSAGES}"
                )

            buffer.extend(chunk)

            while b"\n" in buffer:
                line, _, rest = buffer.partition(b"\n")
                buffer = bytearray(rest)

                if received >= TOTAL_MESSAGES:
                    raise RuntimeError("Extra message received")

                actual = bytes(line) + b"\n"

                if actual != expected[received]:
                    raise AssertionError(
                        f"Mismatch at message {received}"
                    )

                received += 1

                if received % 1000 == 0:
                    print(f"[RECEIVER] {received}/{TOTAL_MESSAGES}")

        thread.join(timeout=5)

        if thread.is_alive():
            raise RuntimeError("Sender did not finish")

        if send_error:
            raise RuntimeError(f"Sender failed: {send_error[0]}")

        if sent != TOTAL_MESSAGES:
            raise AssertionError(
                f"Sent only {sent}/{TOTAL_MESSAGES}"
            )

        print("\n===== TEST PASSED =====")
        print(f"Sent     : {sent}")
        print(f"Received : {received}")


if __name__ == "__main__":
    main()
