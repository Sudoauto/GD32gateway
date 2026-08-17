#!/usr/bin/env python3
"""GD32H759 gateway v0.9 GW-JSONL monitor / management client.

Examples:
  py gateway_uplink_client.py
  py gateway_uplink_client.py --command PING
  py gateway_uplink_client.py --command CFGGET --seconds 2
  py gateway_uplink_client.py --command "MBR,1,0,2"
  py gateway_uplink_client.py --command "CFGSET,POINT,2001,10,temperature,5,0.1,0"

The development image ships with admin / ChangeMe123!. Change it immediately
with PASS,<user>,<new-password> before deployment.
"""
from __future__ import annotations

import argparse
import getpass
import json
import socket
import sys
import time


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Monitor/manage the gateway GW-JSONL uplink")
    p.add_argument("--host", default="192.168.103.213")
    p.add_argument("--port", type=int, default=5001)
    p.add_argument("--user", default="admin")
    p.add_argument("--password", default="ChangeMe123!", help="development default; use --ask-password to avoid shell history")
    p.add_argument("--ask-password", action="store_true")
    p.add_argument("--no-auth", action="store_true", help="only for firmware built with authentication disabled")
    p.add_argument("--command", action="append", default=[], help="command line sent after AUTH; may repeat")
    p.add_argument("--seconds", type=float, default=0.0, help="stop after N seconds; 0 = until Ctrl+C")
    p.add_argument("--raw", action="store_true", help="print raw JSONL instead of pretty JSON")
    return p.parse_args()


def emit(line: bytes, raw: bool) -> None:
    text = line.decode("utf-8", errors="replace").rstrip("\r\n")
    if raw:
        print(text)
        return
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        print(text)
        return
    print(json.dumps(obj, ensure_ascii=False, separators=(", ", ": ")))


def main() -> int:
    args = parse_args()
    if not (1 <= args.port <= 65535):
        print("invalid port", file=sys.stderr)
        return 2
    password = getpass.getpass("Password: ") if args.ask_password else args.password
    deadline = time.monotonic() + args.seconds if args.seconds > 0 else None

    with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
        print(f"connected to {args.host}:{args.port}")
        sock.settimeout(0.5)
        if not args.no_auth:
            auth = f"AUTH,{args.user},{password}"
            sock.sendall(auth.encode("utf-8") + b"\n")
            print(f"> AUTH,{args.user},***")
        for command in args.command:
            command = command.rstrip("\r\n")
            sock.sendall(command.encode("utf-8") + b"\n")
            print(f"> {command}")

        pending = bytearray()
        try:
            while deadline is None or time.monotonic() < deadline:
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    print("gateway closed connection")
                    break
                pending.extend(data)
                while True:
                    pos = pending.find(b"\n")
                    if pos < 0:
                        break
                    line = bytes(pending[: pos + 1])
                    del pending[: pos + 1]
                    emit(line, args.raw)
        except KeyboardInterrupt:
            print("stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
