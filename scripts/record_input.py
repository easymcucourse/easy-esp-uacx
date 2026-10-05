"""Record a named stereo audio input for comparison with playback UART logs.

Requires sounddevice and soundfile in the Python environment used to run it.
No monitor/output stream is opened, preventing a recording feedback loop.
"""
import argparse
import json
import time
import subprocess
import sys
from pathlib import Path

import sounddevice as sd
import soundfile as sf

parser = argparse.ArgumentParser()
parser.add_argument('--input', default='Input 1/2')
parser.add_argument('--seconds', type=float, default=110)
parser.add_argument('--rate', type=int, help='defaults to the input device sample rate')
parser.add_argument('--output', required=True)
parser.add_argument('--serial', help='reset this ESP console and capture its log during recording')
parser.add_argument('--serial-python', default=sys.executable, help='Python with pyserial and esptool installed')
args = parser.parse_args()
devices = sd.query_devices()
matches = [i for i, d in enumerate(devices)
           if args.input.lower() in d['name'].lower() and d['max_input_channels'] >= 2
           and sd.query_hostapis(d['hostapi'])['name'] == 'Windows WASAPI']
if len(matches) != 1:
    raise SystemExit(f'Expected one WASAPI stereo input, found {matches}')
device = matches[0]
if args.rate is None:
    args.rate = int(devices[device]['default_samplerate'])
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
overflows = 0
with sd.InputStream(device=device, channels=2, samplerate=args.rate,
                    dtype='float32', blocksize=1024) as stream:
    started = time.time()
    print(f"Recording {devices[device]['name']} to {output}, start={started:.6f}", flush=True)
    capture = None
    if args.serial:
        capture = subprocess.Popen([args.serial_python, str(Path(__file__).with_name('capture_serial.py')),
                                    '--port', args.serial, '--seconds', str(max(1, args.seconds - 2)),
                                    '--reset', '--output', str(output.with_suffix('.log'))],
                                   stdout=subprocess.DEVNULL)
    remaining = round(args.seconds * args.rate)
    with sf.SoundFile(output, 'w', samplerate=args.rate, channels=2, subtype='FLOAT') as wav:
        while remaining:
            count = min(remaining, 1024)
            data, overflow = stream.read(count)
            overflows += int(overflow)
            wav.write(data)
            remaining -= count
if capture is not None and capture.wait() != 0:
    raise SystemExit('Serial capture failed; recording saved but timing needs verification')
metadata = dict(device=devices[device]['name'], rate=args.rate,
                seconds=args.seconds, started_unix=started, overflows=overflows)
output.with_suffix('.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
print(json.dumps(metadata), flush=True)
if overflows:
    raise SystemExit('Recording overflowed; repeat before drawing audio conclusions')
