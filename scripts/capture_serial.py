"""Capture a bounded UART/USB console run, optionally reset the ESP first."""
import argparse
import time
from pathlib import Path

import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--seconds', type=float, default=40)
parser.add_argument('--output', required=True)
parser.add_argument('--reset', action='store_true')
args = parser.parse_args()
with serial.Serial(args.port, 115200, timeout=0.2) as console:
    console.dtr = False
    console.rts = False
    if args.reset:
        HardReset(console, uses_usb=True)()
    deadline = time.monotonic() + args.seconds
    with Path(args.output).open('wb') as log:
        while time.monotonic() < deadline:
            data = console.read(console.in_waiting or 1)
            if data:
                log.write(data)
                log.flush()
                print(data.decode('utf-8', errors='replace'), end='', flush=True)
