import socket
import threading
import time
import json

HOST = "127.0.0.1"
PORT = 8080

CLIENTS = 20
MESSAGES_PER_CLIENT = 10000
WAIT_SECONDS = 3


def make_message(client_id, i):
    data = {
        "client_id": client_id,
        "id": i,
        "status": "RUNNING",
        "payload": "A" * 900
    }

    return (json.dumps(data) + "\n").encode("utf-8")


def run_client(client_id, results, barrier):
    expected = [
        make_message(client_id, i)
        for i in range(MESSAGES_PER_CLIENT)
    ]

    sent = 0
    received = 0
    send_error = []

    try:
        with socket.create_connection((HOST, PORT), timeout=30) as s:
            s.settimeout(30)

            def sender():
                nonlocal sent

                try:
                    for msg in expected:
                        s.sendall(msg)
                        sent += 1
                except Exception as e:
                    send_error.append(e)

            # 全クライアントの開始タイミングを揃える
            barrier.wait(timeout=30)

            thread = threading.Thread(target=sender, daemon=True)
            thread.start()

            # しばらく応答を読まない
            time.sleep(WAIT_SECONDS)

            buffer = bytearray()

            while received < MESSAGES_PER_CLIENT:
                chunk = s.recv(65536)

                if not chunk:
                    raise RuntimeError("Connection closed unexpectedly")

                buffer.extend(chunk)

                while b"\n" in buffer:
                    line, _, rest = buffer.partition(b"\n")
                    buffer = bytearray(rest)

                    if received >= MESSAGES_PER_CLIENT:
                        raise AssertionError("Extra message")

                    actual = bytes(line) + b"\n"

                    if actual != expected[received]:
                        raise AssertionError(
                            f"Mismatch: client={client_id}, id={received}"
                        )

                    received += 1

            thread.join(timeout=5)

            if thread.is_alive():
                raise RuntimeError("Sender did not finish")

            if send_error:
                raise RuntimeError(f"Sender error: {send_error[0]}")

            if sent != MESSAGES_PER_CLIENT:
                raise AssertionError(f"Sent only {sent} messages")

            print(f"[Client {client_id}] PASS: {received} messages")
            results[client_id] = True

    except Exception as e:
        print(f"[Client {client_id}] FAIL: {e}")
        results[client_id] = False


def main():
    results = [False] * CLIENTS
    threads = []
    barrier = threading.Barrier(CLIENTS)

    for client_id in range(CLIENTS):
        thread = threading.Thread(
            target=run_client,
            args=(client_id, results, barrier)
        )
        threads.append(thread)
        thread.start()

    for thread in threads:
        thread.join()

    print("\n===== TEST RESULT =====")

    if all(results):
        print("ALL CLIENTS PASSED")
    else:
        print("TEST FAILED")

    print(f"Successful clients: {sum(results)}/{CLIENTS}")


if __name__ == "__main__":
    main()
