import socket
import struct
import time

HOST = '127.0.0.1'
PORT = 8080


def connect():
    sock = socket.create_connection((HOST, PORT), timeout=5)
    sock.settimeout(5)
    return sock


def echo(sock, message):
    sock.sendall(message)
    data = b''
    while len(data) < len(message):
        part = sock.recv(len(message) - len(data))
        assert part, 'active connection closed'
        data += part
    assert data == message


def main():
    with connect() as idle, connect() as active, connect() as partial:
        start = time.monotonic()
        # A reset peer must not affect the other clients or the timer scan.
        reset = connect()
        reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        reset.close()
        partial.sendall(b'partial')
        for i in range(5):
            time.sleep(max(0, start + (i + 1) * 10 - time.monotonic()))
            echo(active, b'active\n')
        time.sleep(max(0, start + 58 - time.monotonic()))
        idle.settimeout(0.5)
        try:
            data = idle.recv(1)
        except socket.timeout:
            pass
        else:
            raise AssertionError(f'idle connection closed before 60 seconds: {data!r}')
        echo(active, b'before\n')
        idle.settimeout(5)
        assert idle.recv(1) == b'', 'idle connection was not closed'
        assert time.monotonic() - start >= 60, 'idle connection closed too early'
        partial.settimeout(5)
        assert partial.recv(1) == b'', 'partial input did not time out'
        echo(active, b'after\n')
        with connect() as replacement:
            echo(replacement, b'new client\n')
    print('idle timeout, activity, partial input, reset and multiple clients passed')


if __name__ == '__main__':
    main()
