"""Check a complete public-API playback log; never infer audibility from UART."""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log', type=Path)
parser.add_argument('--require-soak', action='store_true')
parser.add_argument('--emit-caps', action='store_true', help='print transport-tested C table candidates; confirm audibility before use')
args = parser.parse_args()
lines = args.log.read_text(encoding='utf-8', errors='replace').splitlines()
checks = []
capabilities = set()
results = {}
wire = {}
active_wire = None
speed = None
metrics = None
matrix_checked = False
soak_start = None
soak_stats = []
soak_checked = False
runtime_ok = False
unit_ok = False

def check(name, ok):
    checks.append((name, bool(ok)))

def timestamp(line):
    match = re.search(r'\((\d+)\)', line)
    return int(match[1]) if match else None

for line in lines:
    unit = re.search(r'(\d+) Tests (\d+) Failures (\d+) Ignored', line)
    if unit and not unit_ok:
        unit_ok = int(unit[1]) >= 33 and int(unit[2]) == 0 and int(unit[3]) == 0
        check('unit tests', unit_ok)
    if 'RUNTIME SUMMARY' in line:
        runtime_ok = 'failed=0' in line
        check('cross-task validation', runtime_ok)
    if 'DAC ' in line and 'driver=' in line and not matrix_checked:
        capabilities.clear()
        results.clear()
        wire.clear()
        bus = re.search(r'speed=(FS|HS)', line)
        speed = bus[1].lower() if bus else None
    stream = re.search(r'stream PCM input=(\d+) rate=(\d+) .*subslot=(\d+)', line)
    if stream and not matrix_checked:
        active_wire = tuple(map(int, stream.groups()))
    cap = re.search(r'CAP PCM bits=(\d+) rate=(\d+)', line)
    if cap and not matrix_checked:
        capabilities.add((int(cap[1]), int(cap[2])))
    stopped = re.search(r'stream stopped reason=(\d+) errors=(\d+) underruns=(\d+)', line)
    if stopped:
        metrics = tuple(map(int, stopped.groups()))
    result = re.search(r'RESULT bits=(\d+) rate=(\d+) mode=(pull|push) status=(PASS|FAIL)', line)
    if result and not matrix_checked:
        key = (int(result[1]), int(result[2]), result[3])
        # Push's intentional silence after its tail may count as underruns.
        clean = metrics is not None and metrics[1] == 0 and (result[3] == 'push' or metrics[2] == 0)
        results[key] = result[4] == 'PASS' and clean
        wire[key] = active_wire[2] if active_wire and active_wire[:2] == key[:2] else None
    summary = re.search(r'(?<!RUNTIME )SUMMARY passed=(\d+) failed=(\d+) conn=', line)
    if summary and not matrix_checked:
        expected = {(bits, rate, mode) for bits, rate in capabilities for mode in ('pull', 'push')}
        check('complete advertised PCM matrix', bool(expected) and set(results) == expected)
        check('matrix transport and pull underruns', all(results.values()) and bool(results))
        check('matrix summary counts', int(summary[2]) == 0 and int(summary[1]) == len(expected))
        matrix_checked = True
    if 'SOAK start' in line and soak_start is None:
        soak_start = timestamp(line)
    if soak_start is not None and not soak_checked:
        stats = re.search(r'STATS errors=(\d+) underruns=(\d+)', line)
        if stats:
            soak_stats.append(tuple(map(int, stats.groups())))
        if 'SOAK status=' in line:
            end = timestamp(line)
            check('soak reached EOF', 'SOAK status=PASS reason=0' in line)
            check('soak lasted 1800 seconds', end is not None and end - soak_start >= 1800000)
            check('soak samples contain no errors or underruns', len(soak_stats) >= 50 and all(x == (0, 0) for x in soak_stats))
            check('soak terminal metrics', metrics == (0, 0, 0))
            soak_checked = True

check('unit summary present', unit_ok)
check('cross-task summary present', runtime_ok)
check('matrix summary present', matrix_checked)
if args.require_soak:
    check('complete soak record present', soak_checked)
if args.emit_caps:
    check('wire subslots present and consistent', speed is not None and bool(capabilities) and all(
        wire.get((bits, hz, 'pull')) is not None and wire.get((bits, hz, 'pull')) == wire.get((bits, hz, 'push'))
        for bits, hz in capabilities))
for name, ok in checks:
    print(f'{"PASS" if ok else "FAIL"} {name}')
print('Audibility and physical hotplug require separate confirmation.')
if args.emit_caps and all(ok for _, ok in checks):
    groups = {}
    for bits, hz in sorted(capabilities):
        groups.setdefault((bits, wire[(bits, hz, 'pull')]), []).append(hz)
    print('/* Transport-tested candidates. Confirm audibility before registering. */')
    print(f'static const euacx_verified_pcm_t tested_{speed}[] = {{')
    for (bits, slot), rates in groups.items():
        values = ', '.join(map(str, rates))
        print(f'    {{ .bits = {bits}, .subslot = {slot}, .rates = {{ .num_rates = {len(rates)}, .rates = {{ {values} }} }} }},')
    print('};')
raise SystemExit(0 if all(ok for _, ok in checks) else 1)
