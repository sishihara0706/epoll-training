import concurrent.futures
import http.client
import socket
import time

HOST = '127.0.0.1'


def request(data):
    with socket.create_connection((HOST, 9091), timeout=3) as sock:
        sock.settimeout(3)
        for part in data:
            sock.sendall(part)
        response = b''
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                return response
            response += chunk


def metrics():
    conn = http.client.HTTPConnection(HOST, 9091, timeout=3)
    conn.request('GET', '/metrics')
    response = conn.getresponse()
    assert response.status == 200
    assert response.getheader('Content-Type') == 'text/plain; version=0.0.4; charset=utf-8'
    body = response.read().decode()
    conn.close()
    values = {}
    for name, kind in [('epoll_active_connections', 'gauge'), ('epoll_connections_total', 'counter'),
                       ('epoll_bytes_received_total', 'counter'), ('epoll_bytes_sent_total', 'counter'),
                       ('epoll_queued_bytes', 'gauge')]:
        assert '# HELP ' + name + ' ' in body
        assert '# TYPE ' + name + ' ' + kind + '\n' in body
        values[name] = int(next(line.split()[1] for line in body.splitlines() if line.startswith(name + ' ')))
    return values


def echo():
    with socket.create_connection((HOST, 8080), timeout=3) as sock:
        sock.settimeout(3)
        for _ in range(50):
            sock.sendall(b'hello\n')
            data = b''
            while len(data) < 6:
                data += sock.recv(6 - len(data))
            assert data == b'hello\n'


before = metrics()
with socket.create_connection((HOST, 8080), timeout=3) as client:
    client.settimeout(3)
    client.sendall(b'abc\n')
    assert client.recv(4) == b'abc\n'
    current = metrics()
    assert current['epoll_active_connections'] == before['epoll_active_connections'] + 1
    assert current['epoll_connections_total'] == before['epoll_connections_total'] + 1
    assert current['epoll_bytes_received_total'] == before['epoll_bytes_received_total'] + 4
    assert current['epoll_bytes_sent_total'] == before['epoll_bytes_sent_total'] + 4
    assert current['epoll_queued_bytes'] == 0
for _ in range(100):
    if metrics()['epoll_active_connections'] == before['epoll_active_connections']:
        break
    time.sleep(.01)
else:
    raise AssertionError('connection gauge did not decrease')
assert b'200 OK' in request([b'GET /met', b'rics HTTP/1.1\r\nHost: localhost\r\n\r\n'])
assert b'404 Not Found' in request([b'GET /missing HTTP/1.1\r\n\r\n'])
assert b'405 Method Not Allowed' in request([b'POST /metrics HTTP/1.1\r\n\r\n'])
assert b'400 Bad Request' in request([b'garbage\r\n\r\n'])
assert b'400 Bad Request' in request([b'GET /metrics HTTP/1.1\r\ninvalid\r\n\r\n'])
assert b'400 Bad Request' in request([b'x' * 4096])
with socket.create_connection((HOST, 9091), timeout=3) as client:
    client.sendall(b'GET /met')
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    futures = [pool.submit(echo) for _ in range(4)] + [pool.submit(metrics) for _ in range(20)]
    for future in futures:
        future.result()
assert metrics()['epoll_queued_bytes'] == 0
# A stalled request must not occupy a slot indefinitely.
with socket.create_connection((HOST, 9091), timeout=3) as stalled:
    stalled.settimeout(13)
    stalled.sendall(b'GET /metrics HTTP/1.1\r\n')
    assert stalled.recv(1) == b''
assert metrics()['epoll_active_connections'] == before['epoll_active_connections']
print('metrics tests passed')
