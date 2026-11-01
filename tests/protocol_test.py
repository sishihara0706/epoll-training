
import socket

HOST = "127.0.0.1"
PORT = 8080
TIMEOUT = 5


def connect():
    sock = socket.create_connection(
        (HOST, PORT), timeout=TIMEOUT
    )
    sock.settimeout(TIMEOUT)
    return sock


def recv_exact(sock, size):
    """指定バイト数を受信する。"""
    data = bytearray()

    while len(data) < size:
        chunk = sock.recv(size - len(data))

        if not chunk:
            raise AssertionError(
                f"Connection closed: {len(data)}/{size}"
            )

        data.extend(chunk)

    return bytes(data)


def assert_echo(sock, expected):
    actual = recv_exact(sock, len(expected))

    assert actual == expected, (
        f"Expected: {expected!r}\n"
        f"Actual:   {actual!r}"
    )


def test_basic_echo():
    with connect() as sock:
        msg = b"hello\n"
        sock.sendall(msg)
        assert_echo(sock, msg)


def test_split_message():
    with connect() as sock:
        sock.sendall(b"hel")

        # 改行前に応答してはいけない
        sock.settimeout(0.3)

        try:
            data = sock.recv(1)
        except socket.timeout:
            pass  # 期待どおり
        else:
            raise AssertionError(
                f"Unexpected response: {data!r}"
            )

        sock.settimeout(TIMEOUT)

        sock.sendall(b"lo\n")
        assert_echo(sock, b"hello\n")


def test_multiple_messages():
    with connect() as sock:
        msg = b"A\nB\nC\n"
        sock.sendall(msg)
        assert_echo(sock, msg)


def test_binary_message():
    with connect() as sock:
        msg = b"A\x00B\xff\n"
        sock.sendall(msg)
        assert_echo(sock, msg)


def main():
    tests = [
        test_basic_echo,
        test_split_message,
        test_multiple_messages,
        test_binary_message,
    ]

    for test in tests:
        print(f"[RUN] {test.__name__}", flush=True)
        test()
        print(f"[PASS] {test.__name__}", flush=True)

    print(f"\nALL {len(tests)} TESTS PASSED")


if __name__ == "__main__":
    main()

