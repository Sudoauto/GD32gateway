#!/usr/bin/env python3
"""Minimal physical Modbus RTU slave for M1.3/M1.4/M2 board validation.

Requires: pip install pyserial
Example:
    python modbus_rtu_slave.py COM7
    python modbus_rtu_slave.py /dev/ttyUSB0

USB-RS485 adapters with automatic direction control work best.
"""

import argparse
import time
import serial


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def append_crc(payload: bytes) -> bytes:
    c = crc16(payload)
    return payload + bytes((c & 0xFF, (c >> 8) & 0xFF))


def valid_crc(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    return crc16(frame[:-2]) == int.from_bytes(frame[-2:], "little")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--reg0", type=int, default=2300,
                    help="holding register 0; default 2300 -> Point 230.0 with scale 0.1")
    ap.add_argument("--reg1", type=int, default=1234)
    args = ap.parse_args()

    regs = [args.reg0 & 0xFFFF, args.reg1 & 0xFFFF]
    ser = serial.Serial(args.port, args.baud, bytesize=8,
                        parity=serial.PARITY_NONE, stopbits=1,
                        timeout=0.05)
    print(f"Modbus RTU slave {args.slave} on {args.port} @ {args.baud}-8N1")
    print(f"Holding 0={regs[0]}, 1={regs[1]}; Ctrl-C to stop")

    buf = bytearray()
    while True:
        chunk = ser.read(64)
        if chunk:
            buf.extend(chunk)

        # Validation master sends FC03/FC04 fixed 8-byte requests.
        while len(buf) >= 8:
            frame = bytes(buf[:8])
            if not valid_crc(frame):
                del buf[0]
                continue
            del buf[:8]

            slave, fc = frame[0], frame[1]
            addr = (frame[2] << 8) | frame[3]
            qty = (frame[4] << 8) | frame[5]
            print("RX", frame.hex(" ").upper())

            if slave != args.slave:
                continue
            if fc not in (0x03, 0x04):
                resp = append_crc(bytes((slave, fc | 0x80, 0x01)))
            elif qty == 0 or qty > 125:
                resp = append_crc(bytes((slave, fc | 0x80, 0x03)))
            elif addr + qty > len(regs):
                resp = append_crc(bytes((slave, fc | 0x80, 0x02)))
            else:
                data = bytearray()
                for r in regs[addr:addr + qty]:
                    data += bytes(((r >> 8) & 0xFF, r & 0xFF))
                resp = append_crc(bytes((slave, fc, len(data))) + data)

            # Small processing delay, comfortably below the 300 ms timeout.
            time.sleep(0.002)
            ser.write(resp)
            ser.flush()
            print("TX", resp.hex(" ").upper())


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
