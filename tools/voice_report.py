"""The voice lines of a run traced with MZ_NGS2_TRACE=1: each stream a sampler voice played, its
sample count, the parts the game appended, the events after it started and how far it got
("full" when it decoded every sample).

    python tools/autoplay.py --game <folder> --seconds 580 --env MZ_NGS2_TRACE=1 \
        --out out/vtrace $(cat tools/prologue_inputs.txt)
    python tools/voice_report.py out/vtrace/run.log
"""
import re
import sys

log = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
events = {0: 'play', 1: 'stop', 2: 'stop-now', 3: 'kill', 4: 'pause', 5: 'resume'}
voices = {}   # handle -> current play record
plays = []
pending_flags = {}
for n, line in enumerate(log):
    m = re.search(r'voice ([0-9a-f]+) blocks flags (\d+): block 0 offset (\d+) size (\d+) .* samples (\d+)', line)
    if m:
        h, flags = m.group(1), int(m.group(2))
        if flags & 2 and h in voices:
            voices[h]['appends'] += 1
            voices[h]['append_sizes'].append(int(m.group(4)))
        else:
            pending_flags[h] = flags
        continue
    m = re.search(r'voice ([0-9a-f]+) plays (\d+) blocks, first (\d+) bytes at \+\d+ \(skip \d+, (\d+) samples\)', line)
    if m:
        h = m.group(1)
        rec = {'voice': h, 'line': n, 'flags': pending_flags.pop(h, None), 'samples': int(m.group(4)),
               'first_bytes': int(m.group(3)), 'appends': 0, 'append_sizes': [], 'events': [], 'end': None}
        voices[h] = rec
        plays.append(rec)
        continue
    m = re.search(r'voice ([0-9a-f]+) \(sampler \d+\) param 0x00000006 size 12 0000000(\d)', line)
    if m and m.group(1) in voices:
        voices[m.group(1)]['events'].append(events.get(int(m.group(2)), m.group(2)))
        continue
    m = re.search(r'voice ([0-9a-f]+) ended \((\d+) samples decoded\)', line)
    if m and m.group(1) in voices:
        voices[m.group(1)]['end'] = int(m.group(2))
        continue
    m = re.search(r'voice ([0-9a-f]+): ATRAC9 frame rejected', line)
    if m and m.group(1) in voices:
        voices[m.group(1)]['events'].append('REJECTED')

streams = [p for p in plays if p['flags'] and p['flags'] & 1]
print(len(plays), 'plays,', len(streams), 'streams')
for p in streams:
    ev = [e for e in p['events'] if e not in ('pause', 'resume')]
    end = p['end']
    status = 'full' if end == p['samples'] else f"ended at {end}" if end is not None else 'never ended'
    print(f"line {p['line']}: voice {p['voice']} {p['samples']} samples ({p['samples']/48000:.2f} s), first "
          f"{p['first_bytes']} B, {p['appends']} appends {p['append_sizes'][:6]}, events {ev}: {status}")
