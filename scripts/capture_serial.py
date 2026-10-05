"""Capture a bounded UART/USB console run, optionally reset the ESP first."""
import argparse
import time
import sys
from pathlib import Path

import serial
from esptool.reset import HardReset

# UART logs may contain malformed bytes; Windows console encodings vary.
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--seconds', type=float, default=40)
parser.add_argument('--output', required=True)
parser.add_argument('--reset', action='store_true')
args = parser.parse_args()
console = serial.Serial(port=None, baudrate=115200, timeout=0.2)
# Set modem lines before opening: changing default asserted DTR/RTS after
# open can reset CH343-connected ESP boards even without --reset.
console.dtr = False
console.rts = False
console.port = args.port
console.open()
with console:
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
