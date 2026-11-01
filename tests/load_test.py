
import asyncio
import time

HOST = "127.0.0.1"
PORT = 8080

CLIENTS = 100
MESSAGES = 1000
RECONNECT_COUNT = 100
TIMEOUT = 10


async def client_task(client_id):
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(HOST, PORT),
        timeout=TIMEOUT,
    )

    try:
        for i in range(MESSAGES):
            msg = f"client-{client_id}-message-{i}\n"
            expected = msg.encode()

            writer.write(expected)
            await asyncio.wait_for(
                writer.drain(), timeout=TIMEOUT
            )

            response = await asyncio.wait_for(
                reader.readline(), timeout=TIMEOUT
            )

            if response != expected:
                raise AssertionError(
                    f"client {client_id}, message {i}: "
                    f"expected={expected!r}, "
                    f"received={response!r}"
                )
    finally:
        writer.close()
        await asyncio.wait_for(
            writer.wait_closed(), timeout=TIMEOUT
        )


async def test_load():
    await asyncio.gather(
        *(client_task(i) for i in range(CLIENTS))
    )

    print(
        f"[PASS] Load: {CLIENTS} clients, "
        f"{CLIENTS * MESSAGES} messages"
    )


async def test_reconnect():
    for i in range(RECONNECT_COUNT):
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(HOST, PORT),
            timeout=TIMEOUT,
        )

        try:
            expected = f"reconnect-{i}\n".encode()

            writer.write(expected)
            await asyncio.wait_for(
                writer.drain(), timeout=TIMEOUT
            )

            actual = await asyncio.wait_for(
                reader.readline(), timeout=TIMEOUT
            )

            if actual != expected:
                raise AssertionError(
                    f"Reconnect {i}: "
                    f"expected={expected!r}, "
                    f"received={actual!r}"
                )
        finally:
            writer.close()
            await asyncio.wait_for(
                writer.wait_closed(), timeout=TIMEOUT
            )

    print(
        f"[PASS] Reconnect: "
        f"{RECONNECT_COUNT} connections"
    )


async def main():
    start = time.perf_counter()

    print("=== Load Test ===", flush=True)
    await test_load()

    print("=== Reconnect Test ===", flush=True)
    await test_reconnect()

    elapsed = time.perf_counter() - start

    print(f"Elapsed: {elapsed:.3f} sec")
    print("ALL TESTS PASSED")


if __name__ == "__main__":
    asyncio.run(main())

