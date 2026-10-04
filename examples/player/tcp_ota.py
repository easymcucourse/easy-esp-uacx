#!/usr/bin/env python3
"""Upload an ESP-IDF application image to the music player over TCP."""

import argparse
import socket
from pathlib import Path


def read_line(connection: socket.socket) -> str:
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = connection.recv(1)
        if not chunk:
            raise ConnectionError("device closed the connection")
        data.extend(chunk)
    return data.decode("ascii", errors="replace").strip()


def main() -> None:
    parser = argparse.ArgumentParser(description="Upload ESP32 firmware via TCP OTA")
    parser.add_argument("host", help="ESP32 IPv4 address")
    parser.add_argument(
        "firmware",
        type=Path,
        nargs="?",
        default=Path("build/esp32_s3_music_player.bin"),
    )
    parser.add_argument("--port", type=int, default=3333)
    args = parser.parse_args()

    image_size = args.firmware.stat().st_size
    print(f"Uploading {args.firmware} ({image_size} bytes) to {args.host}:{args.port}")
    with args.firmware.open("rb") as image, socket.create_connection(
        (args.host, args.port), timeout=10
    ) as connection:
        connection.settimeout(20)
        connection.sendall(f"OTA {image_size}\n".encode("ascii"))
        while True:
            reply = read_line(connection)
            print(reply)
            if reply == "OTA READY":
                break
            if reply.startswith("OTA ERR") or reply.startswith("ERR "):
                raise RuntimeError(reply)

        uploaded = 0
        while chunk := image.read(4096):
            connection.sendall(chunk)
            uploaded += len(chunk)
            print(f"\rSent {uploaded}/{image_size} bytes", end="", flush=True)
        print()

        while True:
            reply = read_line(connection)
            print(reply)
            if reply.startswith("OTA OK"):
                break
            if reply.startswith("OTA ERR"):
                raise RuntimeError(reply)


if __name__ == "__main__":
    main()
