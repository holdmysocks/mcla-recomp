#!/usr/bin/env python3
"""Receive the live log of a test title (see ps5/title_log.h).

Start this, then launch the title on the console. It retries the connection
until the title is listening, then prints every line as it arrives and appends
it to a file, flushing each time, so the log is on the PC even if the console
goes down mid-line.

    title_log_client.py <console-ip> <output-file> [port] [connect-wait-seconds]
"""
import socket
import sys
import time


def main() -> int:
    host = sys.argv[1]
    path = sys.argv[2]
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 9099
    wait = float(sys.argv[4]) if len(sys.argv) > 4 else 600.0

    deadline = time.time() + wait
    connection = None
    while time.time() < deadline:
        try:
            connection = socket.create_connection((host, port), timeout=2)
            break
        except OSError:
            time.sleep(0.5)
    if connection is None:
        print("title_log_client: the title never listened", flush=True)
        return 2

    connection.settimeout(300)
    with open(path, "ab") as out:
        out.write(b"--- connected\n")
        out.flush()
        try:
            while True:
                data = connection.recv(4096)
                if not data:
                    out.write(b"--- connection closed by the console\n")
                    break
                out.write(data)
                out.flush()
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        except OSError as error:
            out.write(f"--- connection lost: {error}\n".encode())
        out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
