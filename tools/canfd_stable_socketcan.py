#!/usr/bin/env python3
"""Stable CAN-FD 500 kbit/s / BRS-OFF helper for Linux SocketCAN.

Configure the Linux CAN interface first, for example:
  sudo ip link set can0 down
  sudo ip link set can0 type can bitrate 500000 dbitrate 500000 fd on
  sudo ip link set can0 up

This helper deliberately sends ISO CAN-FD frames with bitrate_switch=False.
It never emits BRS traffic, matching the v0.5.0 gateway stable baseline.
"""
import argparse
import time

try:
    import can
except ImportError as exc:
    raise SystemExit("python-can is required: pip install python-can") from exc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default="can0")
    ap.add_argument("--id", type=lambda x: int(x, 0), default=0x301)
    ap.add_argument("--raw", type=int, default=250)
    ap.add_argument("--period", type=float, default=0.5)
    ap.add_argument("--length", type=int, default=12,
                    choices=[0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64])
    args = ap.parse_args()

    if not 0 <= args.id <= 0x7FF:
        raise SystemExit("--id must be a standard CAN ID 0..0x7FF")
    if not 0 <= args.raw <= 0xFFFF:
        raise SystemExit("--raw must be 0..65535")
    if args.period <= 0:
        raise SystemExit("--period must be > 0")

    payload = bytearray(args.length)
    if args.length >= 2:
        payload[0] = (args.raw >> 8) & 0xFF
        payload[1] = args.raw & 0xFF

    bus = can.Bus(interface="socketcan", channel=args.channel, fd=True)
    msg = can.Message(arbitration_id=args.id, is_extended_id=False,
                      is_fd=True, bitrate_switch=False, data=bytes(payload))
    print(f"sending STD 0x{args.id:03X} CAN-FD BRS-OFF len={args.length} "
          f"every {args.period}s; raw={args.raw}")
    next_send = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now >= next_send:
                bus.send(msg, timeout=0.2)
                next_send = now + args.period
            rx = bus.recv(timeout=0.02)
            if rx is not None:
                print("RX:", rx)
    except KeyboardInterrupt:
        print("sender stopped")
    finally:
        bus.shutdown()


if __name__ == "__main__":
    main()
