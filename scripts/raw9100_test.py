#!/usr/bin/env python3
"""Stream a small TSPL label or a caller-supplied printer-data file to RAW 9100."""

import argparse
import socket


TEST_LABEL = (
    b"SIZE 50 mm,25 mm\r\n"
    b"GAP 3 mm,0 mm\r\n"
    b"SPEED 2\r\n"
    b"DENSITY 8\r\n"
    b"DIRECTION 1\r\n"
    b"CLS\r\n"
    b'TEXT 32,32,"2",0,1,1,"RAW TCP TEST"\r\n'
    b"PRINT 1,1\r\n"
)
CHUNK_SIZE = 4096
LARGE_PREFIX_BYTES = 1024 * 1024


def send_bytes(sock: socket.socket, data: bytes) -> int:
    sent = 0
    for offset in range(0, len(data), CHUNK_SIZE):
        chunk = data[offset : offset + CHUNK_SIZE]
        sock.sendall(chunk)
        sent += len(chunk)
    return sent


def main() -> None:
    parser = argparse.ArgumentParser(description="Send print data unchanged to TCP RAW port 9100")
    parser.add_argument("host", help="ESP32 DHCP IPv4 address")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--large", action="store_true", help="stream 1 MiB of blank CRLF lines, then one test label")
    selection.add_argument("--file", help="stream this existing printer-data file unchanged")
    parser.add_argument("--port", type=int, default=9100)
    args = parser.parse_args()

    total = 0
    with socket.create_connection((args.host, args.port), timeout=15) as sock:
        sock.settimeout(30)
        if args.file:
            # The caller is responsible for supplying data suitable for this printer.
            with open(args.file, "rb") as source:
                while True:
                    chunk = source.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    sock.sendall(chunk)
                    total += len(chunk)
        elif args.large:
            blank_lines = b"\r\n" * (CHUNK_SIZE // 2)
            remaining = LARGE_PREFIX_BYTES
            while remaining:
                chunk = blank_lines[: min(len(blank_lines), remaining)]
                sock.sendall(chunk)
                total += len(chunk)
                remaining -= len(chunk)
            total += send_bytes(sock, TEST_LABEL)
        else:
            total += send_bytes(sock, TEST_LABEL)
        sock.shutdown(socket.SHUT_WR)

    print(f"Sent {total} bytes to {args.host}:{args.port}; TCP write side closed")
    print("Check /api/status and the serial log for USB byte totals, then inspect the printer output.")


if __name__ == "__main__":
    main()
