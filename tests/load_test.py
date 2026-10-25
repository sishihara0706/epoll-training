import asyncio
import time

HOST = "127.0.0.1"
PORT = 8080

CLIENTS = 100
MESSAGES = 1000


async def client_task(client_id):
    reader, writer = await asyncio.open_connection(HOST, PORT)

    for i in range(MESSAGES):
        msg = f"client-{client_id}-message-{i}\n"
        writer.write(msg.encode())
        await writer.drain()

        response = await reader.readline()

        if not response:
            print(f"client {client_id}: disconnected")
            break

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


asyncio.run(main())
