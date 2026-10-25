import socket
import threading
import time
import json
import statistics

HOST = "127.0.0.1"
PORT = 8080

NORMAL_CLIENTS = 19
MESSAGES_PER_CLIENT = 1000
SLOW_MAX_MESSAGES = 100000
PAYLOAD_SIZE = 900

stop_event = threading.Event()
slow_ready = threading.Event()


def make_message(client_id, i):
    data = {
        "client_id": client_id,
        "id": i,
        "payload": "A" * PAYLOAD_SIZE
    }
    return (json.dumps(data) + "\n").encode("utf-8")


def recv_line(sock, buffer):
    while b"\n" not in buffer:
        chunk = sock.recv(4096)

        if not chunk:
            raise ConnectionError("Server closed connection")

        buffer.extend(chunk)

    pos = buffer.index(b"\n")
    line = bytes(buffer[:pos + 1])
    del buffer[:pos + 1]

    return line


def slow_client(info):
    sent = 0
    reason = "Maximum messages reached"

    try:
        with socket.create_connection(
            (HOST, PORT), timeout=5
        ) as s:
            # 受信バッファを小さくする
            s.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_RCVBUF,
                4096
            )

            # sendallが詰まった場合の待機上限
            s.settimeout(1.0)
            slow_ready.set()

            try:
                for i in range(SLOW_MAX_MESSAGES):
                    if stop_event.is_set():
                        reason = "Stopped by main"
                        break

                    msg = make_message(999, i)
                    s.sendall(msg)
                    sent += 1

            except socket.timeout:
                reason = "Send timeout (backpressure)"
            except OSError as e:
                reason = f"Socket error: {e}"

            # ここが重要:
            # 送信終了後もソケットを開いたまま、
            # 一度もrecv()せずに待機する
            stop_event.wait(timeout=60)

    except Exception as e:
        reason = f"Connection/setup error: {e}"
        slow_ready.set()

    info["sent"] = sent
    info["reason"] = reason


def normal_client(client_id, results, barrier):
    latencies = []
    count = 0

    try:
        with socket.create_connection(
            (HOST, PORT), timeout=5
        ) as s:
            s.settimeout(15)
            buffer = bytearray()

            # 正常クライアント全員で開始を揃える
            barrier.wait(timeout=15)

            for i in range(MESSAGES_PER_CLIENT):
                expected = make_message(client_id, i)

                start = time.perf_counter()

                s.sendall(expected)
                actual = recv_line(s, buffer)

                elapsed_ms = (
                    time.perf_counter() - start
                ) * 1000

                if actual != expected:
                    raise AssertionError(
                        f"Mismatch: client={client_id}, id={i}"
                    )

                latencies.append(elapsed_ms)
                count += 1

        avg_ms = statistics.mean(latencies)
        max_ms = max(latencies)

        print(
            f"[Normal {client_id:02d}] PASS "
            f"{count} messages | "
            f"avg={avg_ms:.2f}ms "
            f"max={max_ms:.2f}ms"
        )

        results[client_id] = True

    except Exception as e:
        print(f"[Normal {client_id:02d}] FAIL: {e}")
        results[client_id] = False


def main():
    results = [False] * NORMAL_CLIENTS
    slow_info = {}

    slow_thread = threading.Thread(
        target=slow_client,
        args=(slow_info,),
        daemon=True
    )

    print("Starting Slow Client...")
    slow_thread.start()

    if not slow_ready.wait(timeout=10):
        stop_event.set()
        raise RuntimeError("Slow Client startup timeout")

    # Slow Clientに先行して送信させる
    time.sleep(2)

    print("Starting Normal Clients...")

    barrier = threading.Barrier(NORMAL_CLIENTS)
    threads = []

    for client_id in range(NORMAL_CLIENTS):
        thread = threading.Thread(
            target=normal_client,
            args=(client_id, results, barrier)
        )
        threads.append(thread)
        thread.start()

    for thread in threads:
        thread.join()

    # 正常クライアント完了後にSlow Clientを終了
    stop_event.set()
    slow_thread.join(timeout=3)

    print("\n===== TEST RESULT =====")
    print(f"Normal PASS: {sum(results)}/{NORMAL_CLIENTS}")
    print(f"Slow messages sent: {slow_info.get('sent', 'N/A')}")
    print(f"Slow status: {slow_info.get('reason', 'N/A')}")

    if all(results):
        print("TEST PASSED")
    else:
        print("TEST FAILED")
        raise SystemExit(1)


if __name__ == "__main__":
    main()

