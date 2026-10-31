import asyncio
import time

HOST = "127.0.0.1"
PORT = 8080

CLIENTS = 100
MESSAGES = 1000
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
        await writer.wait_closed()


async def main():
    start = time.perf_counter()

    await asyncio.gather(
        *(client_task(i) for i in range(CLIENTS))
    )

    elapsed = time.perf_counter() - start
    total = CLIENTS * MESSAGES

    print(f"clients  : {CLIENTS}")
    print(f"messages : {total}")
    print(f"time     : {elapsed:.3f} sec")
    print(f"rate     : {total / elapsed:.0f} msg/sec")


if __name__ == "__main__":
    asyncio.run(main())

