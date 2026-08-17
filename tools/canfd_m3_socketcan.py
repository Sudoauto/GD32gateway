#!/usr/bin/env python3
"""Compatibility wrapper for the current stable CAN-FD BRS-OFF helper.

The historical M3 helper used BRS.  The production baseline intentionally does
not.  Keep this filename so old instructions/scripts cannot accidentally inject
BRS traffic onto a stable-baseline bus.
"""
from canfd_stable_socketcan import main


if __name__ == "__main__":
    main()
