#!/usr/bin/env python3
"""Simple PC-side validation tool for gateway v0.6.1 TCP echo baseline."""

from __future__ import annotations

import argparse
import os
import socket
import time


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError(f"peer closed after {len(data)}/{size} bytes")
        data.extend(chunk)
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.103.213")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    if not (1 <= args.port <= 65535):
        parser.error("port must be 1..65535")
    if args.rounds <= 0 or args.size <= 0:
        parser.error("rounds and size must be positive")

    total = 0
    start = time.monotonic()
    with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
        sock.settimeout(args.timeout)
        print(f"connected to {args.host}:{args.port}")
        for i in range(args.rounds):
            header = i.to_bytes(4, "big")
            payload = (header + os.urandom(max(0, args.size - len(header))))[: args.size]
            sock.sendall(payload)
            echoed = recv_exact(sock, len(payload))
            if echoed != payload:
                raise RuntimeError(f"echo mismatch at round {i}")
            total += len(payload)
            print(f"round {i + 1}/{args.rounds}: PASS ({len(payload)} bytes)")

    elapsed = max(time.monotonic() - start, 1e-9)
    print(f"TCP echo PASS: {total} bytes, {elapsed:.3f}s, {total / elapsed / 1024:.1f} KiB/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
