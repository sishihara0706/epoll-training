import asyncio
import time

HOST = "127.0.0.1"
PORT = 8080

CLIENTS = 100
MESSAGES = 10000


async def slow_reader_client(client_id):
    reader, writer = await asyncio.open_connection(HOST, PORT)

    for i in range(MESSAGES):
        msg = f"client-{client_id}-{i:05d}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
        writer.write(msg.encode())

        if i % 100 == 0:
            await writer.drain()

    await writer.drain()

    # わざとサーバーからのechoを読まない
    await asyncio.sleep(5)

    writer.close()
    await writer.wait_closed()


async def main():
    start = time.perf_counter()

    await asyncio.gather(
        *(slow_reader_client(i) for i in range(CLIENTS))
    )

    elapsed = time.perf_counter() - start
    print(f"time: {elapsed:.3f} sec")


asyncio.run(main())
